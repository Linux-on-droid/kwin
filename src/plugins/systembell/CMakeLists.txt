
kcoreaddons_add_plugin(SystemBellPlugin INSTALL_NAMESPACE "kwin/plugins")

target_sources(SystemBellPlugin PRIVATE
    main.cpp
    systembell.cpp
)

target_link_libraries(SystemBellPlugin PRIVATE kwin KF6::Notifications)
