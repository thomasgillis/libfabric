#include "rxm.h"

/**
 * @brief copy list of IOVs from/to the host to/from an hmem device
 *
 * inspired by efa_copy_from/to_hmem_iov
 */
ssize_t rxm_copy_hmem_iov(void **desc, char *buf, int buf_size, const struct iovec *hmem_iov,
                          int iov_count, int dir) {
    int i, ret = -1;
    size_t data_size = 0;

    for (i = 0; i < iov_count; i++) {
        if (data_size + hmem_iov[i].iov_len > buf_size) {
            return -FI_ETRUNC;
        }

        ret =
            rxm_copy_hmem(desc[i], buf + data_size, hmem_iov[i].iov_base, hmem_iov[i].iov_len, dir);
        if (ret < 0) return ret;

        data_size += hmem_iov[i].iov_len;
    }
    return data_size;
}

/**
 * @brief copy data from/to the host to/from an hmem device
 *
 * inspired by efa_copy_from/to_hmem
 */
ssize_t rxm_copy_hmem(void *desc, char *host_buf, void *dev_buf, size_t size, int dir) {
    uint64_t device = 0, flags = 0;
    enum fi_hmem_iface iface = FI_HMEM_SYSTEM;
    void *hmem_handle = NULL;

    if (desc) {
        iface = ((struct rxm_mr *)desc)->iface;
        device = ((struct rxm_mr *)desc)->device;
        flags = ((struct rxm_mr *)desc)->hmem_flags;
        hmem_handle = ((struct rxm_mr *)desc)->hmem_handle;
    }

    if (flags & OFI_HMEM_DATA_GDRCOPY_HANDLE) {
        assert(hmem_handle);
        /* TODO: Fine tune the max data size to switch from gdrcopy to cudaMemcpy */
        if (dir == OFI_COPY_IOV_TO_BUF) {
            ofi_hmem_dev_reg_copy_from_hmem(iface, (uint64_t)hmem_handle, host_buf, dev_buf, size);
        } else {
            ofi_hmem_dev_reg_copy_to_hmem(iface, (uint64_t)hmem_handle, dev_buf, host_buf, size);
        }
        return FI_SUCCESS;
    }
    int ret;
    if (dir == OFI_COPY_IOV_TO_BUF) {
        ret = ofi_copy_from_hmem(iface, device, host_buf, dev_buf, size);
    } else {
        ret = ofi_copy_to_hmem(iface, device, dev_buf, host_buf, size);
    }
    return ret;
};
