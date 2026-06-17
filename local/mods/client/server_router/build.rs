fn main() {
    println!("cargo:rustc-link-arg=-Wl,--image-base,0x1D0000000");
    // Link against windows_x86_64_gnu's prebuilt import lib (lowercase kernel32.dll, hint=0).
    // The search path is added by windows_x86_64_gnu's build.rs (pulled in via retour → region → windows-sys).
    // Without this, Wine fails to load the DLL when kernel32.dll isn't yet in the process.
    println!("cargo:rustc-link-lib=windows.0.52.0");
}
