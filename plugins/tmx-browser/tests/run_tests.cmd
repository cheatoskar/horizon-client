@echo off
rem TMX Browser tests, run from the repo root after the build: the logic tests, then the fake-host test for all five games.
set "TD=build\client\test"
set "RC=0"
echo ==== unit tests
"%TD%\tmx_browser_test.exe" || set "RC=1"
for %%g in (eswc sunrise original tmnf tmuf) do (
  echo ==== fake host %%g
  "%TD%\tmx_browser_fakehost.exe" %%g || set "RC=1"
)
exit /b %RC%
