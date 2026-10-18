
@echo off
rem mia with msvc. run it in a "developer command prompt for visual studio"
rem (cl and link on the path; visual studio 2019 16.8 or newer for /std:c11).
rem
rem   build.bat [release|debug]          build\msvc-<config>\: mia.exe and stdlib\ (modules and their plugins)
rem   build.bat [release|debug] check    ...then the tests (tests\programs); they need
rem                                       'sh' on the path, as git for windows has it
rem
rem the same sources as the makefile: msvc has no __int128, so it builds the
rem portable 128-bit integers (include\mia\int128.h), and the windows file of
rem the system layer (source\system\windows.c). build\msvc-<config>\ is an
rem installation folder: mia finds stdlib\ (the modules and their plugins) beside itself.

setlocal
cd /d "%~dp0"

rem the version, as the makefile has it
for /f "tokens=3" %%v in ('findstr /b /c:"VERSION :=" makefile') do set VERSION=%%v

set CONFIG=release
if /i "%~1"=="debug" (set CONFIG=debug& shift) else if /i "%~1"=="release" shift
set ACTION=%~1

set OUT=build\msvc-%CONFIG%

rem warnings: /W4, without the ones about what c11 allows (nameless members, initializers),
rem do-while(0) macros, unused parameters and the implicit integer conversions gcc does not flag either
set FLAGS=/nologo /std:c11 /W4 /wd4100 /wd4127 /wd4201 /wd4204 /wd4244 /wd4267 /wd4706 /utf-8 ^
	/D_CRT_SECURE_NO_WARNINGS /D_CRT_NONSTDC_NO_DEPRECATE /Iinclude "/Dmia_cli_version=\"%VERSION%\""
if "%CONFIG%"=="debug" (set FLAGS=%FLAGS% /Zi /Od /MDd) else (set FLAGS=%FLAGS% /O2 /DNDEBUG /MD)

if not exist %OUT%\objects mkdir %OUT%\objects
if not exist %OUT%\stdlib mkdir %OUT%\stdlib

rem an 8 MiB stack, as linux and macos give: the compiler's passes recurse (as deep as
rem mia_ast_depth_limit allows), and windows gives only 1 MiB
cl %FLAGS% source\*.c source\system\windows.c /Fo%OUT%\objects\ /Fe%OUT%\mia.exe /link /STACK:8388608 || goto failed
copy /y stdlib\*.mia %OUT%\stdlib\ >nul || goto failed

rem the plugins of the standard library, beside its modules: stdlib\<name>.c -> stdlib\<name>.dll.
rem io uses mia's own strings and number formatting: it gets its own copy. the import
rem libraries (.lib, .exp) go to the objects, not into the installation folder
if not exist %OUT%\objects\stdlib\mia mkdir %OUT%\objects\stdlib\mia
cl %FLAGS% /c source\global.c source\arena.c source\view.c source\string.c source\format.c ^
	/Fo%OUT%\objects\stdlib\mia\ || goto failed
cl %FLAGS% /LD stdlib\io.c %OUT%\objects\stdlib\mia\*.obj /Fo%OUT%\objects\stdlib\ /Fe%OUT%\stdlib\io.dll ^
	/link /IMPLIB:%OUT%\objects\stdlib\io.lib || goto failed
cl %FLAGS% /LD stdlib\memory.c /Fo%OUT%\objects\stdlib\ /Fe%OUT%\stdlib\memory.dll ^
	/link /IMPLIB:%OUT%\objects\stdlib\memory.lib || goto failed

rem the example and test plugins: every .c file of examples\plugins\<name>\ or tests\plugins\<name>\,
rem built into examples\plugins\ and tests\plugins\ of the installation folder
for %%k in (examples tests) do (
	if not exist %OUT%\%%k\plugins mkdir %OUT%\%%k\plugins
	for /d %%p in (%%k\plugins\*) do (
		if not exist %OUT%\objects\%%~np mkdir %OUT%\objects\%%~np
		cl %FLAGS% /LD %%p\*.c /Fo%OUT%\objects\%%~np\ /Fe%OUT%\%%k\plugins\%%~np.dll ^
			/link /IMPLIB:%OUT%\objects\%%~np\%%~np.lib || goto failed
	)
)

if not "%ACTION%"=="check" goto done
where sh >nul 2>nul || (echo the tests need 'sh' on the path & goto failed)
sh tests/golden.sh %OUT:\=/%/mia.exe || goto failed

:done
echo built %OUT%\mia.exe
exit /b 0

:failed
echo build.bat: failed
exit /b 1
