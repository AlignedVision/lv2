cmd_../linux_headers//include/linux/mmc/.install := /bin/bash scripts/headers_install.sh ../linux_headers//include/linux/mmc ./include/uapi/linux/mmc ioctl.h; /bin/bash scripts/headers_install.sh ../linux_headers//include/linux/mmc ./include/linux/mmc ; /bin/bash scripts/headers_install.sh ../linux_headers//include/linux/mmc ./include/generated/uapi/linux/mmc ; for F in ; do echo "\#include <asm-generic/$$F>" > ../linux_headers//include/linux/mmc/$$F; done; touch ../linux_headers//include/linux/mmc/.install