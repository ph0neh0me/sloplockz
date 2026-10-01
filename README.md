# sloplockz

`sloplockz` is a small Windows command-line utility for manually holding and observing a level-1 file opportunistic lock. It is intended for filesystem research, software testing, and reproducing time-of-check/time-of-use behavior in controlled environments.

The holder remains in the foreground and keeps the original file handle open. A second process can inspect the state, replace the file contents through that same handle, and release the oplock.

## Build

Use a Visual Studio Developer PowerShell or Developer Command Prompt:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

The executable will be written to `build\Release\sloplockz.exe`.

## Usage

Terminal 1:

```powershell
.\sloplockz.exe hold C:\lab\target.bin
```

Terminal 2:

```powershell
.\sloplockz.exe status C:\lab\target.bin
.\sloplockz.exe write C:\lab\target.bin C:\lab\replacement.bin
.\sloplockz.exe release C:\lab\target.bin
```

Possible states are:

- `held`: the oplock request is active.
- `break-pending`: another process has requested conflicting access and Windows has signaled the holder.
- `released`: the holder closed its file handle.
- `inactive`: no state exists for that path in the current user's temporary directory.

`write` is serviced by the holder process and therefore modifies the target through the already-open handle. Coordination files are stored under `%TEMP%\sloplockz` and keyed by the normalized target path.

## Notes

- Windows only.
- The target must already exist and be writable by the holder.
- `hold` intentionally blocks until `release` is requested.
- Control commands must run as the same Windows user because coordination uses that user's temporary directory.
- Use only on files and systems you own or are authorized to test.

## License

MIT
