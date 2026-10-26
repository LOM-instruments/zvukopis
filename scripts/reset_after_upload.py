# Boots the board into the freshly flashed app after `pio run -t upload`.
#
# This board ends up in download mode after esptool's usual RTS reset, so we use
# a watchdog reset instead. During that reset the USB port disappears, and
# esptool sometimes errors on macOS while closing it even though the reset
# worked. Running it as a separate step lets us ignore that error.
import os
import subprocess

Import("env")


def reset_board(source, target, env):
    esptool = os.path.join(env.PioPlatform().get_package_dir("tool-esptoolpy"), "esptool.py")
    subprocess.call(
        [env.subst("$PYTHONEXE"), esptool, "--chip", "esp32s3", "-p", env.subst("$UPLOAD_PORT"),
         "--after", "watchdog_reset", "chip_id"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    print("Board reset into the app")


env.AddPostAction("upload", reset_board)
