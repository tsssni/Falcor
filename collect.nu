#!/usr/bin/env nu

use run.nu

def main [
    --script: string = "scripts/RTXDIReservoir.py"
    --scene: string = ""
    --output: string = ""
    --offset: int = 0
    --count: int = 64
    --framerate: int = 30
    --resolution: string = "512x512"
    --spp: int = 256
] {
    with-env {
        FALCOR_SCENE: ($scene | path expand)
        FALCOR_OUTPUT_DIR: ($output | path expand)
        FALCOR_START_FRAME: ($offset | into string)
        FALCOR_NUM_FRAMES: ($count | into string)
        FALCOR_FRAMERATE: ($framerate | into string)
        FALCOR_RESOLUTION: $resolution
        FALCOR_SPP: ($spp | into string)
    } {
        run ...["--headless" "--script" ($script | path expand)]
    }
}
