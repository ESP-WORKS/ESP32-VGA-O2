# FabGL is VGA-only in this firmware (no Keyboard / ULP / PS2Controller).
# Patch only if a future FabGL release pulls ULP onto our CLK/DATA pins.

Import("env")  # pylint: disable=undefined-variable

libdeps = env["PROJECT_LIBDEPS_DIR"]
# Nothing to patch: we never instantiate fabgl::Keyboard.
print("[o2em] FabGL host: VGADirectController only (no PS/2 ULP patch)")
