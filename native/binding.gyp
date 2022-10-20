{
    "targets": [
        {
            "target_name": "zstd_proxy",
            "libraries": ["-lzstd"],
            "include_dirs" : ["<!(node -e \"require('nan')\")"],
            "sources": ["../src/zstd-proxy.c", "../src/zstd-proxy-posix.c", "../src/zstd-proxy.addon.cc"],
            "conditions": [
                [
                    'OS=="mac"',
                    {
                        "libraries": ["-L/opt/homebrew/lib"],
                        "include_dirs": ["/opt/homebrew/include"],
                    },
                ],
                [
                    'OS=="linux"',
                    {
                        "libraries": ["-luring"],
                        "sources": ["../src/zstd-proxy-uring.c"],
                    },
                ],
            ],
        }
    ]
}
