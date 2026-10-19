# Example programs (prebuilt guests)

These are the public examples from LEZ's program-deployment tutorial (`examples/program_deployment`),
built at LEZ `dev` commit `f6afdd285d274b3406b93877b16d85fd849c2df3` with
`cargo risczero build --manifest-path examples/program_deployment/methods/guest/Cargo.toml`.
They're embedded into the core library (see `embed_examples.cmake`), so the app can deploy them to
its local chain without a RISC Zero toolchain. `SHA256SUMS` lists the exact bytes.

`tail_call_with_pda` is left out: calling it needs PDA admission evidence, which core_ffi's C API
doesn't offer at this commit (docs/LEZ-V04-NOTES.md §4).

To rebuild after moving the LEZ pin: build the guests as above, copy the four `.bin` files here,
and regenerate `SHA256SUMS`.
