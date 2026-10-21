//! Audio backend (ES8311 / I2S audio sink).
//!
//! Streams PCM audio directly into the C hardware sink (`hal_audio_write`) on a
//! dedicated streaming worker thread. Supports arbitrary `Read` streams as well
//! as local files, and drains the I2S DMA pipeline (`hal_audio_drain`) upon completion.

use std::io::Read;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::Arc;

use pomelo_hal::{AudioBackend, AudioMeta, HalError};

mod ffi {
    extern "C" {
        pub fn hal_audio_init() -> i32;
        pub fn hal_audio_open(sample_rate: u32, channels: u8, bits_per_sample: u8) -> i32;
        pub fn hal_audio_write(data: *const u8, len: u32) -> i32;
        pub fn hal_audio_drain() -> i32;
        pub fn hal_audio_set_volume(volume: u8) -> i32;
        pub fn hal_audio_close() -> i32;
        pub fn hal_audio_configure_pthread(stack_size: u32, use_psram: bool) -> i32;
    }
}

struct PlaybackControl {
    is_playing: AtomicBool,
    is_paused: AtomicBool,
    stop_requested: AtomicBool,
    bytes_played: AtomicU32,
}

pub struct EspAudio {
    meta: Option<AudioMeta>,
    control: Option<Arc<PlaybackControl>>,
    worker_handle: Option<std::thread::JoinHandle<()>>,
    volume: u8,
}

impl EspAudio {
    pub fn new() -> Self {
        unsafe {
            let _ = ffi::hal_audio_init();
            let _ = ffi::hal_audio_set_volume(75);
        }
        Self {
            meta: None,
            control: None,
            worker_handle: None,
            volume: 75,
        }
    }

    /// Play an arbitrary `Read` stream (network stream, memory buffer, TTS),
    /// pushing raw PCM blocks to the hardware audio sink.
    pub fn play_stream<R: Read + Send + 'static>(
        &mut self,
        mut stream: R,
        sample_rate: u32,
        channels: u8,
        bits_per_sample: u8,
    ) -> Result<(), HalError> {
        self.stop();

        let ret = unsafe { ffi::hal_audio_open(sample_rate, channels, bits_per_sample) };
        if ret != 0 {
            return Err(HalError::Internal(ret));
        }

        unsafe {
            let _ = ffi::hal_audio_set_volume(self.volume);
        }

        let control = Arc::new(PlaybackControl {
            is_playing: AtomicBool::new(true),
            is_paused: AtomicBool::new(false),
            stop_requested: AtomicBool::new(false),
            bytes_played: AtomicU32::new(0),
        });

        let control_clone = Arc::clone(&control);

        unsafe {
            let _ = ffi::hal_audio_configure_pthread(32768, true);
        }

        let handle = std::thread::Builder::new()
            .name("audio_stream".into())
            .stack_size(32768)
            .spawn(move || {
                let mut buf = vec![0u8; 2048];
                loop {
                    if control_clone.stop_requested.load(Ordering::Relaxed) {
                        break;
                    }

                    if control_clone.is_paused.load(Ordering::Relaxed) {
                        std::thread::sleep(std::time::Duration::from_millis(15));
                        continue;
                    }

                    match stream.read(&mut buf) {
                        Ok(0) => {
                            // End of stream: drain DMA pipeline before closing
                            unsafe {
                                ffi::hal_audio_drain();
                            }
                            break;
                        }
                        Ok(n) => {
                            let ret = unsafe { ffi::hal_audio_write(buf.as_ptr(), n as u32) };
                            if ret != 0 {
                                std::thread::sleep(std::time::Duration::from_millis(2));
                            } else {
                                control_clone.bytes_played.fetch_add(n as u32, Ordering::Relaxed);
                            }
                        }
                        Err(e) => {
                            eprintln!("[EspAudio] stream read error: {e}");
                            break;
                        }
                    }
                }

                unsafe {
                    ffi::hal_audio_close();
                }
                control_clone.is_playing.store(false, Ordering::Relaxed);
            });

        unsafe {
            let _ = ffi::hal_audio_configure_pthread(0, false);
        }

        self.control = Some(control);
        self.worker_handle = handle.ok();

        Ok(())
    }
}

impl Default for EspAudio {
    fn default() -> Self {
        Self::new()
    }
}

impl EspAudio {
    fn play_internal(&mut self, path: &str, is_loop: bool) -> Result<AudioMeta, HalError> {
        self.stop();

        println!("[EspAudio] streaming play requested for path: '{path}', loop={is_loop}");

        let bytes = std::fs::read(path)
            .map_err(|e| HalError::Io(format!("failed to read '{path}': {e}")))?;

        let ext = std::path::Path::new(path)
            .extension()
            .and_then(|e| e.to_str())
            .unwrap_or("mp3")
            .to_string();

        self.play_bytes_internal(&bytes, Some(&ext), is_loop)
    }

    fn play_bytes_internal(
        &mut self,
        bytes: &[u8],
        format_hint: Option<&str>,
        is_loop: bool,
    ) -> Result<AudioMeta, HalError> {
        self.stop();

        let ext = format_hint.unwrap_or("mp3").to_string();
        let bytes_arc = Arc::new(bytes.to_vec());
        let bytes_clone = Arc::clone(&bytes_arc);
        let ext_clone = ext.clone();

        let mut decoder = pomelo_hal::AudioDecoder::from_bytes((*bytes_arc).clone(), Some(&ext))?;
        let meta = *decoder.meta();

        println!(
            "[EspAudio] Audio meta: {} Hz, {} ch, {}-bit, duration={:.2}s",
            meta.sample_rate, meta.channels, meta.bits_per_sample, meta.duration_secs
        );

        let ret = unsafe {
            ffi::hal_audio_open(
                meta.sample_rate,
                meta.channels,
                meta.bits_per_sample,
            )
        };
        if ret != 0 {
            eprintln!("[EspAudio] ffi::hal_audio_open failed with error code: {ret}");
            return Err(HalError::Internal(ret));
        }

        unsafe {
            let _ = ffi::hal_audio_set_volume(self.volume);
        }

        let control = Arc::new(PlaybackControl {
            is_playing: AtomicBool::new(true),
            is_paused: AtomicBool::new(false),
            stop_requested: AtomicBool::new(false),
            bytes_played: AtomicU32::new(0),
        });

        let control_clone = Arc::clone(&control);

        // Configure pthread worker stack to 64KB in PSRAM:
        // Symphonia's nested frame parsing, Huffman lookup, and DCT synthesis require ample
        // stack space. We configure esp_pthread to allocate the thread stack from PSRAM
        // (MALLOC_CAP_SPIRAM) and pass .stack_size(65536) explicitly so Rust std sets
        // pthread_attr_setstacksize without falling back to ESP-IDF's default 3KB internal stack.
        unsafe {
            let _ = ffi::hal_audio_configure_pthread(65536, true);
        }

        let thread_res = std::thread::Builder::new()
            .name("audio_worker".into())
            .stack_size(65536)
            .spawn(move || {
                println!("[EspAudio] audio_worker thread started (loop={is_loop})");
                let mut buf = vec![0u8; 2048];
                loop {
                    if control_clone.stop_requested.load(Ordering::Relaxed) {
                        println!("[EspAudio] audio_worker received stop request");
                        break;
                    }

                    if control_clone.is_paused.load(Ordering::Relaxed) {
                        std::thread::sleep(std::time::Duration::from_millis(15));
                        continue;
                    }

                    match decoder.read(&mut buf) {
                        Ok(0) => {
                            if is_loop {
                                if control_clone.stop_requested.load(Ordering::Relaxed) {
                                    break;
                                }
                                match pomelo_hal::AudioDecoder::from_bytes((*bytes_clone).clone(), Some(&ext_clone)) {
                                    Ok(new_decoder) => {
                                        decoder = new_decoder;
                                        continue;
                                    }
                                    Err(e) => {
                                        eprintln!("[EspAudio] loop reopen error: {e}");
                                        break;
                                    }
                                }
                            } else {
                                // EOF: drain DMA pipeline before closing
                                println!("[EspAudio] audio_worker reached EOF, draining DMA buffer...");
                                unsafe {
                                    ffi::hal_audio_drain();
                                }
                                break;
                            }
                        }
                        Ok(n) => {
                            let mut written = 0;
                            while written < n {
                                if control_clone.stop_requested.load(Ordering::Relaxed) {
                                    break;
                                }
                                if control_clone.is_paused.load(Ordering::Relaxed) {
                                    std::thread::sleep(std::time::Duration::from_millis(15));
                                    continue;
                                }
                                let to_write = (n - written) as u32;
                                let ret = unsafe {
                                    ffi::hal_audio_write(buf[written..].as_ptr(), to_write)
                                };
                                if ret != 0 {
                                    std::thread::sleep(std::time::Duration::from_millis(2));
                                } else {
                                    control_clone.bytes_played.fetch_add(to_write, Ordering::Relaxed);
                                    written += to_write as usize;
                                }
                            }
                            if control_clone.stop_requested.load(Ordering::Relaxed) {
                                break;
                            }
                        }
                        Err(e) => {
                            eprintln!("[EspAudio] audio_worker read/decode error: {e}");
                            break;
                        }
                    }
                }

                unsafe {
                    ffi::hal_audio_close();
                }
                control_clone.is_playing.store(false, Ordering::Relaxed);
                println!(
                    "[EspAudio] playback thread finished, total bytes played: {}",
                    control_clone.bytes_played.load(Ordering::Relaxed)
                );
            });

        // Restore default pthread configuration on current thread
        unsafe {
            let _ = ffi::hal_audio_configure_pthread(0, false);
        }

        let handle = match thread_res {
            Ok(h) => h,
            Err(e) => {
                eprintln!("[EspAudio] failed to spawn audio_worker thread: {e}");
                unsafe {
                    ffi::hal_audio_close();
                }
                return Err(HalError::Io(format!("spawn audio_worker failed: {e}")));
            }
        };

        self.meta = Some(meta);
        self.control = Some(control);
        self.worker_handle = Some(handle);

        Ok(meta)
    }
}

impl AudioBackend for EspAudio {
    fn play(&mut self, path: &str) -> Result<AudioMeta, HalError> {
        self.play_internal(path, false)
    }

    fn play_loop(&mut self, path: &str) -> Result<AudioMeta, HalError> {
        self.play_internal(path, true)
    }

    fn play_bytes(
        &mut self,
        bytes: &'static [u8],
        format_hint: Option<&str>,
    ) -> Result<AudioMeta, HalError> {
        self.play_bytes_internal(bytes, format_hint, false)
    }

    fn play_bytes_loop(
        &mut self,
        bytes: &'static [u8],
        format_hint: Option<&str>,
    ) -> Result<AudioMeta, HalError> {
        self.play_bytes_internal(bytes, format_hint, true)
    }

    fn play_pcm(
        &mut self,
        pcm: std::sync::Arc<Vec<u8>>,
        sample_rate: u32,
        channels: u8,
        bits_per_sample: u8,
    ) -> Result<AudioMeta, HalError> {
        self.stop();

        let byte_rate = sample_rate * (channels as u32) * ((bits_per_sample as u32) / 8);
        let duration_secs = if byte_rate > 0 {
            pcm.len() as f32 / byte_rate as f32
        } else {
            0.0
        };

        let meta = AudioMeta {
            sample_rate,
            channels,
            bits_per_sample,
            duration_secs,
        };

        let ret = unsafe {
            ffi::hal_audio_open(
                meta.sample_rate,
                meta.channels,
                meta.bits_per_sample,
            )
        };
        if ret != 0 {
            eprintln!("[EspAudio] ffi::hal_audio_open failed with error code: {ret}");
            return Err(HalError::Internal(ret));
        }

        unsafe {
            let _ = ffi::hal_audio_set_volume(self.volume);
        }

        let control = Arc::new(PlaybackControl {
            is_playing: AtomicBool::new(true),
            is_paused: AtomicBool::new(false),
            stop_requested: AtomicBool::new(false),
            bytes_played: AtomicU32::new(0),
        });

        let control_clone = Arc::clone(&control);
        let pcm_clone = Arc::clone(&pcm);

        let thread_res = std::thread::Builder::new()
            .name("audio_pcm".into())
            .stack_size(4096)
            .spawn(move || {
                let mut offset = 0;
                let chunk_size = 2048;

                while offset < pcm_clone.len() {
                    if control_clone.stop_requested.load(Ordering::Relaxed) {
                        break;
                    }

                    if control_clone.is_paused.load(Ordering::Relaxed) {
                        std::thread::sleep(std::time::Duration::from_millis(15));
                        continue;
                    }

                    let end = (offset + chunk_size).min(pcm_clone.len());
                    let to_write = (end - offset) as u32;
                    let ret = unsafe {
                        ffi::hal_audio_write(pcm_clone[offset..end].as_ptr(), to_write)
                    };
                    if ret != 0 {
                        std::thread::sleep(std::time::Duration::from_millis(2));
                    } else {
                        control_clone.bytes_played.fetch_add(to_write, Ordering::Relaxed);
                        offset = end;
                    }
                }

                if !control_clone.stop_requested.load(Ordering::Relaxed) {
                    unsafe {
                        ffi::hal_audio_drain();
                    }
                }

                unsafe {
                    ffi::hal_audio_close();
                }
                control_clone.is_playing.store(false, Ordering::Relaxed);
            });

        let handle = thread_res.ok();

        self.meta = Some(meta);
        self.control = Some(control);
        self.worker_handle = handle;

        Ok(meta)
    }

    fn pause(&mut self) {
        if let Some(ref c) = self.control {
            c.is_paused.store(true, Ordering::Relaxed);
        }
    }

    fn resume(&mut self) {
        if let Some(ref c) = self.control {
            c.is_paused.store(false, Ordering::Relaxed);
        }
    }

    fn stop(&mut self) {
        if let Some(ref c) = self.control {
            c.stop_requested.store(true, Ordering::Relaxed);
            c.is_playing.store(false, Ordering::Relaxed);
        }
        if let Some(handle) = self.worker_handle.take() {
            let _ = handle.join();
        }
        unsafe {
            ffi::hal_audio_close();
        }
    }

    fn set_volume(&mut self, volume: u8) {
        self.volume = volume.min(100);
        unsafe {
            let _ = ffi::hal_audio_set_volume(self.volume);
        }
    }

    #[inline]
    fn volume(&self) -> u8 {
        self.volume
    }

    #[inline]
    fn is_playing(&self) -> bool {
        if let Some(ref c) = self.control {
            c.is_playing.load(Ordering::Relaxed) && !c.is_paused.load(Ordering::Relaxed)
        } else {
            false
        }
    }

    #[inline]
    fn position_secs(&self) -> f32 {
        if let (Some(ref meta), Some(ref c)) = (&self.meta, &self.control) {
            let byte_rate = meta.sample_rate * (meta.channels as u32) * ((meta.bits_per_sample as u32) / 8);
            if byte_rate > 0 {
                let bytes = c.bytes_played.load(Ordering::Relaxed);
                return (bytes as f32 / byte_rate as f32).min(meta.duration_secs);
            }
        }
        0.0
    }

    #[inline]
    fn tick(&mut self) {
        // No-op! State is updated atomically by the audio streaming worker thread
        // upon reaching EOF and completing DMA drain.
    }
}
