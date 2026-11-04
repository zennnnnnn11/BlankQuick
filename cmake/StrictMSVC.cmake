set(strict_msvc_options
    /Wall /WX /permissive- /options:strict /utf-8 /diagnostics:caret
    /sdl /GS /guard:cf /guard:ehcont /Qspectre /fp:strict /volatile:iso
    /EHsc /EHr
    /Zc:__cplusplus /Zc:preprocessor /Zc:externConstexpr /Zc:inline
    /Zc:throwingNew /Zc:strictStrings /Zc:referenceBinding /Zc:rvalueCast
    /Zc:ternary /Zc:lambda /Zc:implicitNoexcept /Zc:threadSafeInit
    /Zc:alignedNew /Zc:char8_t /Zc:checkGwOdr /Zc:templateScope
    /Zc:enumTypes /Zc:tlsGuards /Zc:wchar_t
    /external:W0 /external:env:INCLUDE /analyze /analyze:external- /analyze:pluginEspXEngine.dll
    /wd4711 /wd5045
    "$<$<CONFIG:Release>:/wd4710>"
    "/analyze:ruleset${PROJECT_SOURCE_DIR}/cmake/StrictNative.ruleset"
)
