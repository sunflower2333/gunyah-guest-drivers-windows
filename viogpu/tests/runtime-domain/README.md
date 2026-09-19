# Native runtime queue integration

The 58557 candidate combines the 58556 primary-read cache with retained native
allocation domains, destroy preflight validation, and Mesa runtime protocol v2.
Both the D3D Mesa submodule and the separately packaged OpenGL/Vulkan ICD use
Mesa `709ac5ef875a50c1793d5741eec5025b43c88d2c`. The latter is built by Mesa run
`35454133031`; changing only the D3D submodule would leave ordinary Vulkan loader
clients without the new runtime protocol.

Run `python viogpu/tests/runtime-domain/run.py` and its `unretained`,
`wrong-generation`, and `early-close` negative controls. The signed packaging
workflow also runs the Mesa runtime queue ownership/drain tests and their three
negative controls. These exercise actual production helpers with test shims;
they do not establish real Windows D3D12 runtime admission.

Target validation must retain the baseline shared-resource Regression/Core/Async
cases, verify ordinary Vulkan/OpenGL loading from the installed package, and
then run the matched VKD3D shared-graphics probe from commit
`a823c15140f5e71580b0f47f76086da97118080f` (CI `35453533088`). Runtime header SHA256
must be `facd7a43c42b227b9bc9cc44d184b4801b3e5201168028fb9ae9700f8abd1999`.
The shared-graphics probe still does not substitute for ordinary D3D12CreateDevice,
DXGI presentation, full feature-level support, or application acceptance.

Native scanout remains opt-in; this integration does not resolve its outstanding
lease/unmap/detach lifetime requirements or claim a frame-rate improvement.
