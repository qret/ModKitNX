# project.cmake - the ONE place for the application's name, version and author.
#
# Read by:
#   CMakeLists.txt          include(project.cmake)  -> NACP, .nro name, APP_* defines
#   the local build/release scripts   a regex over the set(...) lines below
# Keep each value on its own line in exactly this form:  set(NAME "value")
# The scripts parse these lines; anything fancier (variables, generator expressions)
# will not be understood by them.
#
# APP_NAME     - name in the Homebrew Menu, the .nro file name, the SD folder sdmc:/switch/<APP_NAME>/,
#                the release asset <APP_NAME>.nro. The name is final (author qret, repository qret/ModKitNX);
#                change it only together with the repository.
# APP_VERSION  - X.Y.Z only. The release tag is v<APP_VERSION>; the release script refuses a tag that is
#                already taken and checks that the built .nro carries this exact version in its NACP.
# APP_AUTHOR   - shown in the Homebrew Menu (NACP author field).

set(APP_NAME    "ModKitNX")
set(APP_VERSION "1.0.5")
set(APP_AUTHOR  "qret")
