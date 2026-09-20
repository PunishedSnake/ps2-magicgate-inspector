# P0 real-hardware campaign v2

This campaign supersedes the *test pack*, not the prior artifacts. Every upstream
artifact remains pinned by ID, run ID, source SHA and GitHub digest.

## Upstream packs

### Historical reference

- workflow run: `32707959479` (#541)
- source SHA: `a81ca03dce237fbbb48c52ce39adc3319cc0fea1`
- artifact ID: `9512980209`
- digest: `sha256:c3e05adc3c24861eb3144265e40999c3297822cc3924eff0ec1eb306121da250`

### FINAL-SAFE v1

- run: `33910509140`
- source SHA: `54b1e6e8a3bbe79652390198bec4917e18738471`
- artifact ID: `9951188483`
- digest: `sha256:66a61f2c8f27576dfa93e227ea96a3ab53e07b67eb63a9518fda7f24334515bc`

### FINAL-SAFE v2

- run: `33914313656`
- source SHA: `445ddd31efc6f9c88f2652dcfafee6ca8ff32808`
- artifact ID: `9952574690`
- digest: `sha256:2edfd7e36cd2d056b64af286c0de58042a790d2897ed8f1db41f3c0fcddd6a09`

### FINAL-ASYNC v2

- run: `33914598481`
- source SHA: `a8fae27a9a8da3aa0ff015065429e6b6ddebe26b`
- artifact ID: `9952702690`
- digest: `sha256:52cd486e3d4ccd8b4993f215969c465eed30fa0a21a0417e5125a5508e074bf5`

## Test order

1. smoke/correctness all candidate families;
2. isolate FINAL-SAFE v1 -> v2 on `conflict32` selected-save import;
3. select v2 conflict batch 16 / 32 / 64;
4. on the winning batch compare same-source sync / async-read / async-write;
5. compare final winner against BASE and ORIGINAL-541;
6. whole-system re-profile after the winner is chosen.

Correctness is always a hard gate. Report p50/p95/p99/max and operation-level
results, not only averages.
