function(blank_native_quality target)
    target_compile_options(${target} PRIVATE /W4 /WX /permissive- /utf-8 /Zc:__cplusplus
        /Zc:preprocessor /sdl /guard:cf /guard:ehcont
        /w14242 /w14254 /w14263 /w14265 /w14287 /w14296 /w14311 /w14545
        /w14546 /w14547 /w14549 /w14555 /w14619 /w14826 /w14905 /w14906 /w14928)
    target_compile_definitions(${target} PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN
        PRIVATE QT_NO_KEYWORDS QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII
        QT_NO_FOREACH QT_NO_CONTEXTLESS_CONNECT QT_ENABLE_STRICT_MODE_UP_TO=0x060A00
        QT_DISABLE_DEPRECATED_UP_TO=0x060A00 QT_WARN_DEPRECATED_UP_TO=0x060C00)
    target_compile_features(${target} PUBLIC cxx_std_20)
    target_link_options(${target} PRIVATE /WX /guard:cf /guard:ehcont /DYNAMICBASE
        /NXCOMPAT /HIGHENTROPYVA /CETCOMPAT)
    set_target_properties(${target} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF CXX_SCAN_FOR_MODULES OFF)
endfunction()

