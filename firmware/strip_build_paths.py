Import("env")
import os

# See the comment above `extra_scripts` in platformio.ini. Computed from
# the current build machine's own home directory, never hardcoded, so
# this works (and never leaks anyone's username) regardless of who's
# actually running the build.
home = os.path.expanduser("~").replace(os.sep, "/")
env.Append(CCFLAGS=[f"-ffile-prefix-map={home}=/pio"])
