# Building on Windows

The Visual Studio project assumes that you have an integrated vcpkg installation.

Run `vcpkg install ffmpeg:x64-windows-static libass:x64-windows-static` or
`vcpkg install ffmpeg:x86-windows-static libass:x86-windows-static`
to install the required libraries depending on which configuration you want to compile.

Open and compile with the Visual Studio solution. Done!