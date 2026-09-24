# Turns the ststm32 bare-metal build into one for the ARM7TDMI of the CUBe.
#
# PlatformIO has no AT91SAM7 platform. ststm32 is used for its arm-none-eabi
# toolchain and its generic ELF -> BIN builder, but its frameworks/_bare.py
# assumes a Cortex-M and adds flags this device must not get:
#
#   -mthumb          the makefile builds everything, board_cstartup.S and the
#                    interrupt handlers included, in ARM state
#   -fdata-sections  CUBE*_flash.lds collects *(.data) and *(.bss) only; the
#                    per-variable .data.* / .bss.* sections would be orphans
#   --relax, F_CPU   not set by the makefile either
#
# _bare.py runs after every pre: script, and the objects are bound to their
# flags before any post: script runs, so neither can simply remove them. A
# build middleware can: it is called per source file, after _bare.py.

Import("env")

DROP = {"-mthumb", "-fdata-sections", "-Wl,--gc-sections,--relax"}


def _keep(flags):
    return [f for f in flags if f not in DROP]


def _fix_program_env(prog_env):
    # The program is linked and size-checked with the main environment, and
    # both read it only when they run, so fixing it here is early enough.
    prog_env.Replace(
        # CUBE*_flash.lds names its sections .fixed (code, rodata) and
        # .relocate (initialised data, copied to RAM), not .text / .data
        SIZEPROGREGEXP=r"^(?:\.fixed|\.ARM\.extab|\.ARM\.exidx|\.relocate)\s+(\d+).*",
        SIZEDATAREGEXP=r"^(?:\.relocate|\.bss)\s+(\d+).*",
        LINKFLAGS=_keep(prog_env.get("LINKFLAGS", []))
        + ["-nostartfiles", "-Wl,--gc-sections"],
        # the makefile links -lc -lm; libgcc comes with the driver
        LIBS=["c", "m"],
    )


def at91sam7_object(obj_env, node):
    if not env.get("_AT91SAM7_PROGRAM_FIXED"):
        _fix_program_env(env)
        env["_AT91SAM7_PROGRAM_FIXED"] = True

    asppflags = list(obj_env.get("ASPPFLAGS", []))
    if node.get_path().endswith(".S"):
        asppflags.append("-D__ASSEMBLY__")

    return obj_env.Object(
        node,
        ASFLAGS=_keep(obj_env.get("ASFLAGS", [])),
        ASPPFLAGS=asppflags,
        CCFLAGS=_keep(obj_env.get("CCFLAGS", [])),
        CPPDEFINES=[d for d in obj_env.get("CPPDEFINES", [])
                    if not (isinstance(d, tuple) and d[0] == "F_CPU")],
    )


env.AddBuildMiddleware(at91sam7_object)
