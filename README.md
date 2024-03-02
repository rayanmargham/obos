# OBOS
## Building
### Prerequisites
- xorriso
- An x86_64-elf gcc toolchain. Preferably the latest version
- Nasm
- CMake
- Git (Not optional, even if you download the repository)
- Ninja
- Optionally, qemu to run the ISO generated by the build.
All these tools are avaliable on windows if you look well enough. Building for windows should always work.
On Linux, use your package manager to install these packages if you don't have them already.
### The build process
1. Clone the repository using git, or download it.
```
git clone https://github.com/oberrow/obos.git
```
3. Open a terminal inside the directory the repostory was downloaded into.
4. Run these commands to build:<br></br>
For x86_64:
```sh
cmake -GNinja -DCMAKE_BUILD_TYPE=Debug --toolchain=src/toolchains/x86_64/toolchain.cmake .
ninja -j0
```
4. To run the newly built iso in qemu, run the appropriate script for your Host OS and Guest OS under the scripts directory.
