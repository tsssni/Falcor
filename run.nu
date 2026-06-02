#!/usr/bin/env nu

export def main [...args] {
    let ver = open --raw /proc/driver/nvidia/version
    | into string
    | parse --regex '(?:Module|x86_64|aarch64)\s\s+(?<v>[0-9.]+)'
    | get v.0
    cd ($env.FILE_PWD | path join build linux-clang bin Debug)
    run-external $"nixVulkanNvidia-($ver)" ./Mogwai ...$args
}
