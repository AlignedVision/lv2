cmd_../linux_headers//include/linux/hdlc/.install := /bin/bash scripts/headers_install.sh ../linux_headers//include/linux/hdlc ./include/uapi/linux/hdlc ioctl.h; /bin/bash scripts/headers_install.sh ../linux_headers//include/linux/hdlc ./include/linux/hdlc ; /bin/bash scripts/headers_install.sh ../linux_headers//include/linux/hdlc ./include/generated/uapi/linux/hdlc ; for F in ; do echo "\#include <asm-generic/$$F>" > ../linux_headers//include/linux/hdlc/$$F; done; touch ../linux_headers//include/linux/hdlc/.install