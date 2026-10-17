
@echo off
rem mc with msvc. run it from c\ in a "developer command prompt for visual
rem studio" (cl, lib and link on the path; visual studio 2019 16.8 or newer
rem for /std:c11).
rem
rem   build.bat [release|debug]          build\msvc-<config>\mc.exe and the example plugins
rem   build.bat [release|debug] test     ...then run the unit tests
rem   build.bat [release|debug] check    ...then the unit tests and, when 'sh' is on the
rem                                       path (git for windows), the golden tests
rem
rem the same sources as the makefile: msvc has no __int128, so it builds the
rem portable 128-bit integers (include\mc\int128.h), and the windows half
rem of the platform layer (source\platform_windows.c).

setlocal
cd /d "%~dp0"

set CONFIG=release
if /i "%~1"=="debug" (set CONFIG=debug& shift) else if /i "%~1"=="release" shift
set ACTION=%~1

set OUT=build\msvc-%CONFIG%
rem the standard library and the plugins as absolute paths, with '/' (a c string needs no escapes then)
set "STDLIB=%~dp0..\stdlib"
set "STDLIB=%STDLIB:\=/%"
set "PLUGINS=%~dp0%OUT%\plugins"
set "PLUGINS=%PLUGINS:\=/%"

rem warnings: /W4, without the ones about what c11 allows (nameless members, initializers),
rem do-while(0) macros, unused parameters and the implicit integer conversions gcc does not flag either
set FLAGS=/nologo /std:c11 /W4 /wd4100 /wd4127 /wd4201 /wd4204 /wd4244 /wd4267 /wd4706 /utf-8 ^
	/D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_DEPRECATE /Iinclude
if "%CONFIG%"=="debug" (set FLAGS=%FLAGS% /Zi /Od /MDd) else (set FLAGS=%FLAGS% /O2 /DNDEBUG /MD)
set DEFINES="/DMC_DEFAULT_STDLIB=\"%STDLIB%\"" "/DMC_TEST_PLUGINS=\"%PLUGINS%\""

if not exist %OUT%\library mkdir %OUT%\library
if not exist %OUT%\tests mkdir %OUT%\tests
if not exist %OUT%\plugins mkdir %OUT%\plugins

rem the library: every source but main.c
cl %FLAGS% %DEFINES% /c source\*.c source\plugins\*.c /Fo%OUT%\library\ || goto failed
move /y %OUT%\library\main.obj %OUT%\main.obj >nul || goto failed
lib /nologo /out:%OUT%\mc.lib %OUT%\library\*.obj || goto failed
cl %FLAGS% %OUT%\main.obj %OUT%\mc.lib /Fe%OUT%\mc.exe || goto failed

rem a plugin is every .c file of examples\plugins\<name>\, as one dll
for /d %%p in (examples\plugins\*) do (
	if not exist %OUT%\plugins\%%~np mkdir %OUT%\plugins\%%~np
	cl %FLAGS% /LD %%p\*.c /Fo%OUT%\plugins\%%~np\ /Fe%OUT%\plugins\%%~np.dll || goto failed
)

if "%ACTION%"=="" goto done

cl %FLAGS% %DEFINES% /Itests\cairo tests\*.c %OUT%\mc.lib /Fo%OUT%\tests\ /Fe%OUT%\mc_tests.exe || goto failed
%OUT%\mc_tests.exe || goto failed
if not "%ACTION%"=="check" goto done
where sh >nul 2>nul || (echo golden tests skipped: no 'sh' on the path & goto done)
sh tests/golden.sh %OUT:\=/%/mc.exe || goto failed

:done
echo built %OUT%\mc.exe
exit /b 0

:failed
echo build.bat: failed
exit /b 1
