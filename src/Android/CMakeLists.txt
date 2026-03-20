if(NOT ANDROID)
    return()
endif()

# Always compile Android initialization and interface
target_sources(${CMAKE_PROJECT_NAME}
    PRIVATE
        AndroidInit.cc
        AndroidInterface.cc
        AndroidInterface.h
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Only compile custom serial implementation if NOT using Qt standard SerialPort
if(NOT DEFINED QGC_USE_QT_SERIAL_ON_ANDROID)
    message(STATUS "Using custom Android serial implementation (AndroidSerial + qtandroidserialport)")
    
    target_sources(${CMAKE_PROJECT_NAME}
        PRIVATE
            AndroidSerial.cc
            AndroidSerial.h
    )
    
    add_subdirectory(qtandroidserialport)
else()
    message(STATUS "Skipping custom Android serial implementation (using Qt6::SerialPort)")
endif()
