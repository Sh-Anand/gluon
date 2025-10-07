use std::io;
use std::io::ErrorKind;
use std::os::fd::{AsRawFd, OwnedFd, RawFd};
use std::ptr::NonNull;

/// Manages a shared memory mapping backed by a memfd.
#[derive(Debug)]
pub struct SharedMemoryRegion {
    _fd: OwnedFd,
    ptr: NonNull<u8>,
    size: usize,
}

unsafe impl Send for SharedMemoryRegion {}
unsafe impl Sync for SharedMemoryRegion {}

impl SharedMemoryRegion {
    /// Create a mapping from an existing memfd owned by another process.
    pub fn from_owned_fd(fd: OwnedFd, base: usize) -> io::Result<Self> {
        let size = file_size(fd.as_raw_fd())?;
        if size == 0 {
            return Err(io::Error::new(
                ErrorKind::InvalidInput,
                "shared memory fd has zero length",
            ));
        }
        if base == 0 {
            return Err(io::Error::new(
                ErrorKind::InvalidInput,
                "shared memory base missing",
            ));
        }

        let ptr = map_shared_region(fd.as_raw_fd(), size, Some(base))
            .ok_or_else(io::Error::last_os_error)?;

        Ok(SharedMemoryRegion {
            _fd: fd,
            // SAFETY: map_shared_region never returns null pointer on success
            ptr: unsafe { NonNull::new_unchecked(ptr) },
            size,
        })
    }
}

impl Drop for SharedMemoryRegion {
    fn drop(&mut self) {
        unsafe {
            let _ = libc::munmap(self.ptr.as_ptr().cast(), self.size);
        }
    }
}

fn map_shared_region(fd: std::os::fd::RawFd, size: usize, preferred_base: Option<usize>) -> Option<*mut u8> {
    const PREFERRED_BASES: &[usize] = &[0x1000_0000, 0x2000_0000, 0x3000_0000, 0x4000_0000];

    let prot = libc::PROT_READ | libc::PROT_WRITE;

    if let Some(base) = preferred_base {
        unsafe {
            let fixed_flag = map_fixed_noreplace_flag();
            let mut flags = libc::MAP_SHARED;
            if fixed_flag != 0 {
                flags |= fixed_flag;
            } else {
                flags |= libc::MAP_FIXED;
            }
            let desired = base as *mut libc::c_void;
            let mapped = libc::mmap(desired, size, prot, flags, fd, 0);
            if mapped != libc::MAP_FAILED {
                return Some(mapped.cast());
            }
        }
    }

    #[cfg(any(target_os = "linux"))]
    unsafe {
        let fixed_flag = map_fixed_noreplace_flag();
        if fixed_flag != 0 {
            for &base in PREFERRED_BASES {
                let desired = base as *mut libc::c_void;
                let mapped = libc::mmap(desired, size, prot, libc::MAP_SHARED | fixed_flag, fd, 0);
                if mapped != libc::MAP_FAILED {
                    return Some(mapped.cast());
                }
            }
        }

        #[cfg(target_arch = "x86_64")]
        {
            let mapped = libc::mmap(
                std::ptr::null_mut(),
                size,
                prot,
                libc::MAP_SHARED | libc::MAP_32BIT,
                fd,
                0,
            );
            if mapped != libc::MAP_FAILED {
                return Some(mapped.cast());
            }
        }
    }

    unsafe {
        let mapped = libc::mmap(std::ptr::null_mut(), size, prot, libc::MAP_SHARED, fd, 0);
        if mapped == libc::MAP_FAILED {
            None
        } else {
            Some(mapped.cast())
        }
    }
}

#[cfg(any(target_os = "linux"))]
const fn map_fixed_noreplace_flag() -> libc::c_int {
    #[cfg(any(target_env = "gnu", target_env = "musl"))]
    {
        libc::MAP_FIXED_NOREPLACE
    }

    #[cfg(not(any(target_env = "gnu", target_env = "musl")))]
    {
        0
    }
}

fn file_size(fd: RawFd) -> io::Result<usize> {
    unsafe {
        let mut stat: libc::stat = std::mem::zeroed();
        if libc::fstat(fd, &mut stat) != 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(stat.st_size as usize)
    }
}
