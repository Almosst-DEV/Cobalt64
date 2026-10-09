Build (macOS, Rust >= 1.87):
    git clone https://github.com/steelbrain/metal2vulkan
    cd metal2vulkan && git checkout 43c46ac8a24adf1a6e872b8a52c706ec9614fad0
    git apply /path/to/Navi48-MacOS/third-party/metal2vulkan-n48/patches/0001-metal2vulkan-changes-for-n48xlate.patch
    cp -R /path/to/Navi48-MacOS/third-party/metal2vulkan-n48/n48xlate .
    cargo build --release -p n48xlate     # target/release/libn48xlate.dylib
See NOTICE for the licence (LGPL-3.0-or-later; separate from the MIT code in this repository).
