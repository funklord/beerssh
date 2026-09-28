#ifndef BSSH_RANGE_SOURCE_ANDROID_H
#define BSSH_RANGE_SOURCE_ANDROID_H

/*
 * Makes the font download's byte-range source Android's own HTTPS
 * (BsshHttp.java) rather than QNetworkAccessManager, which has no TLS
 * backend on Android. Called once, as the platform starts.
 */
void bssh_install_android_range_source();

#endif /* BSSH_RANGE_SOURCE_ANDROID_H */
