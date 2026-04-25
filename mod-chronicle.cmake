# Extra link dependencies for mod-chronicle.
# Included inline by modules/CMakeLists.txt after all modules are processed.
# zlib is used for in-memory gzip compression in InstanceTracker::UploadAndDelete.
target_link_libraries(modules PUBLIC zlib)
