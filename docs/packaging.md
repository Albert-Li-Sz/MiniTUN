# Packaging

DEB and RPM component packages are scheduled for stage 14. The named CMake presets are
reserved now so that CI and developer workflows remain stable, but stage 0 does not
claim to generate installable packages.

The final package split will be `minitun-client` and `minitun-server`, with state retained
across normal upgrades and removals.
