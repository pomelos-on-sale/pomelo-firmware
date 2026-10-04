//! Audio backend (ES8311 / I2S audio sink).
//!
//! Streams PCM audio directly into the C hardware sink (`hal_audio_write`) on a
//! dedicated streaming worker thread. Supports arbitrary `Read` streams as well
//! as local files, and drains the I2S DMA pipeline (`hal_audio_drain`) upon completion.

use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use std::sync::Arc;

use pomelo_hal::wav::{parse_wav_header, WavMetadata};
use pomelo_hal::{AudioBackend, AudioMeta, HalError};

mod ffi {
    extern "C" {
        pub fn hal_audio_init() -> i32;
        pub fn hal_audio_open(sample_rate: u32, channels: u8, bits_per_sample: u8) -> i32;
        pub fn hal_audio_write(data: *const u8, len: u32) -> i32;
        pub fn hal_audio_drain() -> i32;
        pub fn hal_audio_set_volume(volume: u8) -> i32;
        pub fn hal_audio_close() -> i32;
    }
}

struct PlaybackControl {
    is_playing: AtomicBool,
    is_paused: AtomicBool,
    stop_requested: AtomicBool,
    bytes_played: AtomicU32,
}

pub struct EspAudio {
    meta: Option<WavMetadata>,
    control: Option<Arc<PlaybackControl>>,
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
        self.control = Some(control);

        let _ = std::thread::Builder::new()
            .name("audio_stream".into())
            .spawn(move || {
                let mut buf = [0u8; 4096];
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
                        Err(_) => break,
                    }
                }

                unsafe {
                    ffi::hal_audio_close();
                }
                control_clone.is_playing.store(false, Ordering::Relaxed);
            });

        Ok(())
    }
}

impl Default for EspAudio {
    fn default() -> Self {
        Self::new()
    }
}

impl AudioBackend for EspAudio {
    fn play(&mut self, path: &str) -> Result<AudioMeta, HalError> {
        self.stop();

        let mut file =
            File::open(path).map_err(|e| HalError::Io(format!("failed to open '{path}': {e}")))?;
        let mut header_buf = [0u8; 512];
        let bytes_read = file
            .read(&mut header_buf)
            .map_err(|e| HalError::Io(e.to_string()))?;
        let meta = parse_wav_header(&header_buf[..bytes_read]).map_err(HalError::Io)?;

        let ret = unsafe {
            ffi::hal_audio_open(
                meta.sample_rate,
                meta.channels as u8,
                meta.bits_per_sample as u8,
            )
        };
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

        let data_offset = meta.data_offset as u64;
        let control_clone = Arc::clone(&control);

        let _ = std::thread::Builder::new()
            .name("audio_worker".into())
            .spawn(move || {
                if file.seek(SeekFrom::Start(data_offset)).is_err() {
                    control_clone.is_playing.store(false, Ordering::Relaxed);
                    unsafe {
                        ffi::hal_audio_close();
                    }
                    return;
                }

                let mut buf = [0u8; 4096];
                loop {
                    if control_clone.stop_requested.load(Ordering::Relaxed) {
                        break;
                    }

                    if control_clone.is_paused.load(Ordering::Relaxed) {
                        std::thread::sleep(std::time::Duration::from_millis(15));
                        continue;
                    }

                    match file.read(&mut buf) {
                        Ok(0) => {
                            // EOF: drain DMA pipeline before closing
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
                        Err(_) => break,
                    }
                }

                unsafe {
                    ffi::hal_audio_close();
                }
                control_clone.is_playing.store(false, Ordering::Relaxed);
            });

        let audio_meta = AudioMeta {
            sample_rate: meta.sample_rate,
            channels: meta.channels as u8,
            bits_per_sample: meta.bits_per_sample as u8,
            duration_secs: meta.duration_secs,
        };

        self.meta = Some(meta);
        self.control = Some(control);

        Ok(audio_meta)
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
            if meta.byte_rate > 0 {
                let bytes = c.bytes_played.load(Ordering::Relaxed);
                return (bytes as f32 / meta.byte_rate as f32).min(meta.duration_secs);
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
