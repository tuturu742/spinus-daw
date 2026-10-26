# Measures sample files as the sound index does (session/SoundDescriptors), one JSON line each, for choosing
# and checking the starter sample pack (tools/starter-pack/make_pack.py says how). Not run by CI.
add_executable (spinus_describe_samples EXCLUDE_FROM_ALL DescribeSamples.cpp)
target_link_libraries (spinus_describe_samples PRIVATE spinus_core spinus_warnings)
