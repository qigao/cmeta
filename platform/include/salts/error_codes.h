#ifndef SALTS_ERROR_CODES_H
#define SALTS_ERROR_CODES_H

#define SALTS_OK 0

/* Dependency-free status codes shared by lower-level Salts modules. */
#define SALTS_ERROR_CODE_ITEMS(X)                                                                 \
  X(SALTS_EAI_ADDRFAMILY, -3000, "SALTS_EAI_ADDRFAMILY", "address family for hostname not supported") \
  X(SALTS_EAI_AGAIN, -3001, "SALTS_EAI_AGAIN", "temporary DNS failure")                              \
  X(SALTS_EAI_BADFLAGS, -3002, "SALTS_EAI_BADFLAGS", "bad DNS flags")                                \
  X(SALTS_EAI_FAIL, -3003, "SALTS_EAI_FAIL", "non-recoverable DNS failure")                          \
  X(SALTS_EAI_FAMILY, -3004, "SALTS_EAI_FAMILY", "address family not supported by DNS result")        \
  X(SALTS_EAI_MEMORY, -3005, "SALTS_EAI_MEMORY", "out of memory during DNS lookup")                  \
  X(SALTS_EAI_NODATA, -3006, "SALTS_EAI_NODATA", "no DNS data")                                     \
  X(SALTS_EAI_NONAME, -3007, "SALTS_EAI_NONAME", "hostname not found")                               \
  X(SALTS_EAI_SERVICE, -3008, "SALTS_EAI_SERVICE", "service not available for socket type")           \
  X(SALTS_EAI_SOCKTYPE, -3009, "SALTS_EAI_SOCKTYPE", "socket type not supported")                    \
  X(SALTS_EAI_SYSTEM, -3010, "SALTS_EAI_SYSTEM", "system error during DNS lookup")                   \
  X(SALTS_EAI_CANCELED, -3011, "SALTS_EAI_CANCELED", "DNS lookup canceled")                          \
  X(SALTS_EADDRINUSE, -4000, "SALTS_EADDRINUSE", "address already in use")                           \
  X(SALTS_EADDRNOTAVAIL, -4001, "SALTS_EADDRNOTAVAIL", "address not available")                      \
  X(SALTS_EAFNOSUPPORT, -4002, "SALTS_EAFNOSUPPORT", "address family not supported")                \
  X(SALTS_EALREADY, -4003, "SALTS_EALREADY", "operation already in progress")                        \
  X(SALTS_EBADF, -4004, "SALTS_EBADF", "bad file descriptor")                                      \
  X(SALTS_EBUSY, -4005, "SALTS_EBUSY", "resource busy or locked")                                    \
  X(SALTS_ECANCELED, -4006, "SALTS_ECANCELED", "operation canceled")                                 \
  X(SALTS_ECHARSET, -4007, "SALTS_ECHARSET", "invalid character set")                                \
  X(SALTS_ECONNABORTED, -4008, "SALTS_ECONNABORTED", "connection aborted")                          \
  X(SALTS_ECONNREFUSED, -4009, "SALTS_ECONNREFUSED", "connection refused")                          \
  X(SALTS_ECONNRESET, -4010, "SALTS_ECONNRESET", "connection reset by peer")                        \
  X(SALTS_EDESTADDRREQ, -4011, "SALTS_EDESTADDRREQ", "destination address required")                \
  X(SALTS_EFAULT, -4012, "SALTS_EFAULT", "bad address")                                             \
  X(SALTS_EFBIG, -4013, "SALTS_EFBIG", "file too large")                                            \
  X(SALTS_EHOSTUNREACH, -4014, "SALTS_EHOSTUNREACH", "host is unreachable")                         \
  X(SALTS_EINTR, -4015, "SALTS_EINTR", "operation interrupted")                                     \
  X(SALTS_EINVAL, -4016, "SALTS_EINVAL", "invalid argument")                                        \
  X(SALTS_EIO, -4017, "SALTS_EIO", "I/O error")                                                     \
  X(SALTS_EISCONN, -4018, "SALTS_EISCONN", "socket is already connected")                           \
  X(SALTS_EISDIR, -4019, "SALTS_EISDIR", "is a directory")                                         \
  X(SALTS_ELOOP, -4020, "SALTS_ELOOP", "too many symbolic links")                                  \
  X(SALTS_EMFILE, -4021, "SALTS_EMFILE", "too many open files")                                     \
  X(SALTS_EMSGSIZE, -4022, "SALTS_EMSGSIZE", "message too long")                                    \
  X(SALTS_ENAMETOOLONG, -4023, "SALTS_ENAMETOOLONG", "name too long")                               \
  X(SALTS_ENETDOWN, -4024, "SALTS_ENETDOWN", "network is down")                                     \
  X(SALTS_ENETUNREACH, -4025, "SALTS_ENETUNREACH", "network is unreachable")                        \
  X(SALTS_ENFILE, -4026, "SALTS_ENFILE", "too many open files in system")                           \
  X(SALTS_ENOBUFS, -4027, "SALTS_ENOBUFS", "no buffer space available")                             \
  X(SALTS_ENODEV, -4028, "SALTS_ENODEV", "no such device")                                          \
  X(SALTS_ENOENT, -4029, "SALTS_ENOENT", "no such file or directory")                               \
  X(SALTS_ENOMEM, -4030, "SALTS_ENOMEM", "not enough memory")                                       \
  X(SALTS_ENONET, -4031, "SALTS_ENONET", "machine is not on the network")                           \
  X(SALTS_ENOPROTOOPT, -4032, "SALTS_ENOPROTOOPT", "protocol option not available")                 \
  X(SALTS_ENOSPC, -4033, "SALTS_ENOSPC", "no space left on device")                                 \
  X(SALTS_ENOSYS, -4034, "SALTS_ENOSYS", "function not implemented")                                \
  X(SALTS_ENOTCONN, -4035, "SALTS_ENOTCONN", "socket is not connected")                             \
  X(SALTS_ENOTDIR, -4036, "SALTS_ENOTDIR", "not a directory")                                       \
  X(SALTS_ENOTEMPTY, -4037, "SALTS_ENOTEMPTY", "directory not empty")                               \
  X(SALTS_ENOTSOCK, -4038, "SALTS_ENOTSOCK", "not a socket")                                        \
  X(SALTS_ENOTSUP, -4039, "SALTS_ENOTSUP", "operation not supported")                               \
  X(SALTS_EPERM, -4040, "SALTS_EPERM", "operation not permitted")                                   \
  X(SALTS_EPIPE, -4041, "SALTS_EPIPE", "broken pipe")                                               \
  X(SALTS_EPROTO, -4042, "SALTS_EPROTO", "protocol error")                                          \
  X(SALTS_EPROTONOSUPPORT, -4043, "SALTS_EPROTONOSUPPORT", "protocol not supported")                 \
  X(SALTS_EPROTOTYPE, -4044, "SALTS_EPROTOTYPE", "protocol wrong type for socket")                   \
  X(SALTS_ERANGE, -4045, "SALTS_ERANGE", "result too large")                                        \
  X(SALTS_EROFS, -4046, "SALTS_EROFS", "read-only file system")                                     \
  X(SALTS_ESHUTDOWN, -4047, "SALTS_ESHUTDOWN", "cannot send after transport endpoint shutdown")      \
  X(SALTS_ESPIPE, -4048, "SALTS_ESPIPE", "invalid seek")                                            \
  X(SALTS_ESRCH, -4049, "SALTS_ESRCH", "no such process")                                           \
  X(SALTS_ETIMEDOUT, -4050, "SALTS_ETIMEDOUT", "operation timed out")                               \
  X(SALTS_ETXTBSY, -4051, "SALTS_ETXTBSY", "text file busy")                                        \
  X(SALTS_EXDEV, -4052, "SALTS_EXDEV", "cross-device link")                                         \
  X(SALTS_UNKNOWN, -4053, "SALTS_UNKNOWN", "unknown error")                                          \
  X(SALTS_EAUTH, -4054, "SALTS_EAUTH", "authentication failed")                                      \
  X(SALTS_EOF, -4095, "SALTS_EOF", "end of file")

#define SALTS_ERROR_CODE_ITEM_DECL(name, value, name_text, message_text) name = value,
enum { SALTS_ERROR_CODE_ITEMS(SALTS_ERROR_CODE_ITEM_DECL) };
#undef SALTS_ERROR_CODE_ITEM_DECL

#endif /* SALTS_ERROR_CODES_H */
