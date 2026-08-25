# =============================================================================
# tool/Makefile -- the repo's own tooling
#
# Nothing here is compiled; these are the checks that keep conventions true
# mechanically rather than by review. Kept as a Makefile so the tools have
# the same interface as everything else in the tree, and so CI and a
# pre-commit hook call the same target a person would.
#
# TARGETS
#   make check    -- run every gate below (what CI runs)
#   make indent   -- indentation gate only (code-style.md sec 2)
#   make fix      -- rewrite this project's sources to the indent
#                    convention. Not run by CI: fixing is a decision, not
#                    a gate.
#   make fonts    -- placeholder; the bundled-font validation this must run
#                    arrives with the curated set at M9 (project.md sec
#                    9.6), and is a build-FAILING check by design.
# =============================================================================

SRC_DIRS = ../src

check: indent

indent:
	./check-indent.sh

fix:
	python3 tabify.py $$(find $(SRC_DIRS) -name '*.cpp' -o -name '*.h')

fonts:
	@echo "tools: font validation lands with the curated set at M9"
	@echo "tools:   see project.md sec 9.6 -- it must FAIL the build, not warn"

.PHONY: check indent fix fonts
