{
    "targets": [
        {
            "target_name": "gpu-metrics",
            "sources": ["gpu_metrics.cc"],
            "include_dirs": [
                "<!@(node -p \"require('node-addon-api').include\")"
            ],
            "defines": ["NAPI_DISABLE_CPP_EXCEPTIONS"],
            "libraries": ["pdh.lib", "dxgi.lib"],
            "conditions": [
                [
                    "OS==\"win\"",
                    {
                        "msvs_settings": {
                            "VCCLCompilerTool": {
                                "AdditionalOptions": ["/std:c++20"]
                            }
                        }
                    }
                ]
            ]
        }
    ]
}
