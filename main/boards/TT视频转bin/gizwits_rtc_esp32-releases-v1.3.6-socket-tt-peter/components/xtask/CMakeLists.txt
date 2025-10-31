set(SRC_FILES "xtask.c")

set(COMPONENT_SRCS ${SRC_FILES})
set(COMPONENT_ADD_INCLUDEDIRS .)
set(COMPONENT_REQUIRES freertos)

register_component()

