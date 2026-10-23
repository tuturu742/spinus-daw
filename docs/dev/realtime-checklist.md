# Real-time safety checklist

Required for every pull request that touches `src/engine/` or `src/dsp/` (AGENTS.md rule 1).
Copy the list into the PR description and tick each item, or explain why it does not apply.

The **audio thread** is any code reachable from a JUCE/Tracktion audio callback:
`AudioIODeviceCallback`, `AudioProcessor::processBlock`, Tracktion `Node::process`,
plugin `process()` calls, and anything they call.

## On the audio thread, the code must not

- [ ] **Allocate or free memory.** No `new`/`delete`, `malloc`, `std::vector::push_back` past capacity,
      `std::string`/`juce::String` construction or concatenation, `std::function` creation,
      `juce::Array::add` past capacity, or `std::make_shared`.
- [ ] **Lock.** No `std::mutex`, `juce::CriticalSection`, `juce::ScopedLock`, `std::condition_variable`,
      or anything that might block. Spin-try (`tryLock` that never waits) is allowed only with a fallback.
- [ ] **Do I/O.** No file, network, socket or pipe access, including `juce::File` methods.
- [ ] **Log or print.** No `DBG`, `std::cout`, `juce::Logger`, `printf`.
- [ ] **Make system calls** that can block: no `sleep`, thread creation, `MessageManager::callAsync`
      (it allocates), timers, or async updaters triggered from the audio thread.
- [ ] **Touch the model.** No `juce::ValueTree` reads or writes, no Command API calls.
- [ ] **Call Python or any AI code.**
- [ ] **Throw exceptions** or rely on RTTI-heavy dispatch in the hot path.

## The code must

- [ ] Use **lock-free FIFOs** (`juce::AbstractFifo`, Tracktion's `LockFreeFifo`, rigtorp SPSC) or
      atomics to exchange data between the audio thread and other threads.
- [ ] **Preallocate** everything in `prepareToPlay` / `prepare` / node initialisation, sized for the
      maximum block size.
- [ ] Hand objects that must be deleted back to a non-real-time thread for destruction.
- [ ] Handle any block size from 1 to the maximum, and sample rates 44.1–192 kHz.
- [ ] Produce no denormals (`juce::ScopedNoDenormals` at the top of processing).
- [ ] Be bit-identical across runs given the same input, unless randomness is intended and seeded.

## Evidence in the PR

- [ ] Unit or golden-audio test covering the new processing path.
- [ ] The Debug + AddressSanitizer CI job passes.
- [ ] For new DSP: CPU cost measured at 48 kHz / 128 samples and noted in the PR.
