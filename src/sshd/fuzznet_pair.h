#ifndef BSSH_FUZZNET_PAIR_H
#define BSSH_FUZZNET_PAIR_H

#include <QString>
#include <QStringList>

/*
 * A device for BeerSSH, paired to this fuzznet host in one step (project.md
 * sec 5.6): `beersshd --fuzznet-pair --fuzznet-dir=DIR ...`.
 *
 * fuzznet's own design for a small device is that the key exchange happens
 * off it, and what it keeps is an image of the result (its device/device.h:
 * "the image is the manual key exchange"). So the device is paired here, in a
 * store of its own made for the purpose and removed after: its prekey, the
 * host's card for it, the acceptance, and the image -- which is printed, for
 * the BeerSSH host editor's "Device" field. Each step is fuzznet's, run as
 * `beersshd --fuzznet`, since fuzznet's node runs once per process.
 *
 * `program` is this beersshd; `options` are fuzznet's, --fuzznet-dir= naming
 * the host's store, and the service and product numbers where it uses them.
 * The image, or empty with `why` saying what stopped it.
 */
QString bssh_fuzznet_pair(const QString &program, const QStringList &options, QString *why);

#endif
