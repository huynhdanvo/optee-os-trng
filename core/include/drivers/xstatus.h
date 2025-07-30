#define XST_SUCCESS                     0L
#define XST_FAILURE                     1L
#define XST_DEVICE_NOT_FOUND            2L
#define XST_DEVICE_BLOCK_NOT_FOUND      3L
#define XST_INVALID_VERSION             4L
#define XST_DEVICE_IS_STARTED           5L
#define XST_DEVICE_IS_STOPPED           6L
#define XST_FIFO_ERROR                  7L	/*!< An error occurred during an
						   operation with a FIFO such as
						   an underrun or overrun, this
						   error requires the device to
						   be reset */
#define XST_RESET_ERROR                 8L	/*!< An error occurred which requires
						   the device to be reset */
#define XST_DMA_ERROR                   9L	/*!< A DMA error occurred, this error
						   typically requires the device
						   using the DMA to be reset */
#define XST_NOT_POLLED                  10L	/*!< The device is not configured for
						   polled mode operation */
#define XST_FIFO_NO_ROOM                11L	/*!< A FIFO did not have room to put
						   the specified data into */
#define XST_BUFFER_TOO_SMALL            12L	/*!< The buffer is not large enough
						   to hold the expected data */
#define XST_NO_DATA                     13L	/*!< There was no data available */
#define XST_REGISTER_ERROR              14L	/*!< A register did not contain the
						   expected value */
#define XST_INVALID_PARAM               15L	/*!< An invalid parameter was passed
						   into the function */
#define XST_NOT_SGDMA                   16L	/*!< The device is not configured for
						   scatter-gather DMA operation */
#define XST_LOOPBACK_ERROR              17L	/*!< A loopback test failed */
#define XST_NO_CALLBACK                 18L	/*!< A callback has not yet been
						   registered */
#define XST_NO_FEATURE                  19L	/*!< Device is not configured with
						   the requested feature */
#define XST_NOT_INTERRUPT               20L	/*!< Device is not configured for
						   interrupt mode operation */
#define XST_DEVICE_BUSY                 21L	/*!< Device is busy */
#define XST_ERROR_COUNT_MAX             22L	/*!< The error counters of a device
						   have maxed out */
#define XST_IS_STARTED                  23L	/*!< Used when part of device is
						   already started i.e.
						   sub channel */
#define XST_IS_STOPPED                  24L	/*!< Used when part of device is
						   already stopped i.e.
						   sub channel */
#define XST_DATA_LOST                   26L	/*!< Driver defined error */
#define XST_RECV_ERROR                  27L	/*!< Generic receive error */
#define XST_SEND_ERROR                  28L	/*!< Generic transmit error */
#define XST_NOT_ENABLED                 29L	/*!< A requested service is not
						   available because it has not
						   been enabled */
#define XST_NO_ACCESS			30L	/* Generic access error */
#define XST_TIMEOUT                     31L	/*!< Event timeout occurred */
#define XST_GLITCH_ERROR		32L     /*!< Used when a glitch occurs*/