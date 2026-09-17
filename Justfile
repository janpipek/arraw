set windows-powershell := true

# List available tasks
default:
    @just --list

# Configure the Debug build tree (build/debug); cheap to re-run
configure:
    cmake --preset debug

# Configure and build everything (Debug)
build: configure
    cmake --build --preset debug

# Build and run the GUI application
run: configure
    cmake --build --preset debug --target arraw-ui
    ./build/debug/arraw-ui

# Build and run the CLI
cli *args: configure
    cmake --build --preset debug --target arraw-cli
    ./build/debug/arraw-cli {{args}}

# Configure and build Release (build/release)
release:
    cmake --preset release
    cmake --build --preset release

# Remove the build trees
clean:
    cmake -E rm -rf build
