#define KERNEL_BUFFER_SIZE (2 * 1024 * 1024) // 2 MiB

/**
 * idma_port_type_t - type of the port used for the transfer
 *
 * This enum describes the type of the port used for the transfer.
 * It is used to determine how the source and destination addresses are handled.
 *
 * IDMA_KERNEL_BUFFER: The source or destination buffer is a kernel buffer (buffer that can be mapped to user space)
 * IDMA_COPY_BUFFER: The source or destination buffer is copied into a kernel buffer before transfer
 * IDMA_USER_DIRECT: The user space mapping is used to get the physical address for the transfer.
 *                   Please note that the entire source or destination buffer/range must be physically contiguous.
 *                   Additionally, this will only work if physical address = dma address
 *                   Not imeplemented yet.
 */
typedef enum
{
        IDMA_KERNEL_BUFFER = 0,
        IDMA_COPY_BUFFER = 1,
        IDMA_USER_DIRECT = 2,
} idma_port_type_t;

/**
 * idma_memcpy_transfer_t - structure for 2D transfers
 *
 * This structure is used to describe a 2D iDMA transfer operation
 *
 * @src:        The source address as user virtual memory address
 *              Ignored if src_type is IDMA_KERNEL_BUFFER
 * @dst:        The destination address as user virtual memory address
 *              Ignored if dst_type is IDMA_KERNEL_BUFFER
 * @src_type:   The type of the source address
 * @dst_type:   The type of the destination address
 * @length:     The number of bytes to transfer
 * @conf:       Configuration bits, e.g. for decoupling reads and writes. Please refer to the iDMA documentation
 */
typedef struct idma_memcpy_transfer
{
        uintptr_t src;
        uintptr_t dst;
        idma_port_type_t src_type;
        idma_port_type_t dst_type;
        size_t length;
        size_t conf;
} idma_memcpy_transfer_t;

struct idma_proxy_kernel_buffer
{
        uint8_t buffer[KERNEL_BUFFER_SIZE];
} __attribute__((aligned(4096)));

#define IOCTL_MAGIC '{' // 0x7b
#define IOCTL_ISSUE_MEMCPY_TRANSFER _IOW(IOCTL_MAGIC, 1, idma_memcpy_transfer_t *)
