#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/leds.h>
#include <linux/kfifo.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <asm/uaccess.h>

#define DEVICE_NAME  "morse-code"

#define INTER_WORD_DOTTIMES 7
#define INTER_LETTER_DOTTIMES 3

#define BITS_IN_A_BYTE 8
#define ONES_IN_A_DOT 1
#define ONES_IN_A_DASH 3
#define DOT_SYMBOL '.'
#define DASH_SYMBOL '-'
#define SEPARATOR_SYMBOL ' '

#define QUEUE_SIZE (1 << 15)

// Morse Encoding description:
// - msb to be output first, followed by 2nd msb... (left to right)
// - each bit gets one "dot" time.
// - "dashes" are encoded here as being 3 times as long as "dots". Therefore
//   a single dash will be the bits: 111.
// - ignore trailing 0's (once last 1 output, rest of 0's ignored).
// - Space between dashes and dots is one dot time, so is therefore encoded
//   as a 0 bit between two 1 bits.
//
// Example:
//   R = dot   dash   dot       -- Morse code
//     =  1  0 111  0  1        -- 1=LED on, 0=LED off
//     =  1011 101              -- Written together in groups of 4 bits.
//     =  1011 1010 0000 0000   -- Pad with 0's on right to make 16 bits long.
//     =  B    A    0    0      -- Convert to hex digits
//     = 0xBA00                 -- Full hex value (see value in table below)
//
// Between characters, must have 3-dot times (total) of off (0's) (not encoded here)
// Between words, must have 7-dot times (total) of off (0's) (not encoded here).
static const unsigned short letter_to_morsecode_bits_map[] = {
	0xB800,	// A 1011 1
	0xEA80,	// B 1110 1010 1
	0xEBA0,	// C 1110 1011 101
	0xEA00,	// D 1110 101
	0x8000,	// E 1
	0xAE80,	// F 1010 1110 1
	0xEE80,	// G 1110 1110 1
	0xAA00,	// H 1010 101
	0xA000,	// I 101
	0xBBB8,	// J 1011 1011 1011 1
	0xEB80,	// K 1110 1011 1
	0xBA80,	// L 1011 1010 1
	0xEE00,	// M 1110 111
	0xE800,	// N 1110 1
	0xEEE0,	// O 1110 1110 111
	0xBBA0,	// P 1011 1011 101
	0xEEB8,	// Q 1110 1110 1011 1
	0xBA00,	// R 1011 101
	0xA800,	// S 1010 1
	0xE000,	// T 111
	0xAE00,	// U 1010 111
	0xAB80,	// V 1010 1011 1
	0xBB80,	// W 1011 1011 1
	0xEAE0,	// X 1110 1010 111
	0xEBB8,	// Y 1110 1011 1011 1
	0xEEA0	// Z 1110 1110 101
};

DEFINE_LED_TRIGGER(led_trigger);
static DECLARE_KFIFO(flashed_codes_queue, char, QUEUE_SIZE);
static DEFINE_MUTEX(queue_mutex);

#define driver_print(logLevel, message, ...) printk(logLevel DEVICE_NAME ": " message, ##__VA_ARGS__)

/******************************************************
 * Parameter
 ******************************************************/
#define MIN_DOT_TIME 1
#define MAX_DOT_TIME 2000
#define DEFAULT_DOT_TIME 200
static int dottime = DEFAULT_DOT_TIME;

// Declare the variable as a parameter.
//   S_IRUGO makes its /sys/module node readable.
//   # cat /sys/module/demo_paramdrv/parameters/dottime
module_param(dottime, int, S_IRUGO);
MODULE_PARM_DESC(dottime, " Sets the timing of the morse code \"dot\", in ms."
                 " Range is 1 to 2000.");

/******************************************************
 * Helper and Processing Functions
 ******************************************************/

static int queue_lock(void)
{
	return mutex_lock_interruptible(&queue_mutex);
}

static void queue_unlock(void)
{
	mutex_unlock(&queue_mutex);
}

static int put_valid_morsecode_into_queue(const int consecutive_ones_seen)
{
	if ((consecutive_ones_seen == ONES_IN_A_DOT) ||
	        (consecutive_ones_seen == ONES_IN_A_DASH)) {
		if (queue_lock()) {
			return -EFAULT;
		}
		if (consecutive_ones_seen == ONES_IN_A_DOT) {
			kfifo_put(&flashed_codes_queue, DOT_SYMBOL);
		} else if (consecutive_ones_seen == ONES_IN_A_DASH) {
			kfifo_put(&flashed_codes_queue, DASH_SYMBOL);
		}
		queue_unlock();
	}
	return 0;
}

static int flash_morsecode(unsigned short morsecode)
{
	int consecutive_ones_seen = 0;
	// iterate through each bit in bitstring, from left to right
	while (morsecode != 0) {
		unsigned char leftmost_bit =
		    (morsecode >> (sizeof(unsigned short) * BITS_IN_A_BYTE - 1));

		if (leftmost_bit == 1) {
			led_trigger_event(led_trigger, LED_FULL);
			consecutive_ones_seen++;
		} else {
			if (put_valid_morsecode_into_queue(consecutive_ones_seen)) {
				return -EFAULT;
			}
			led_trigger_event(led_trigger, LED_OFF);
			consecutive_ones_seen = 0;
		}
		msleep(dottime);
		morsecode <<= 1;
	}
	if (put_valid_morsecode_into_queue(consecutive_ones_seen)) {
		return -EFAULT;
	}
	led_trigger_event(led_trigger, LED_OFF);
	msleep(dottime);
	return 0;
}

static int process_morsecode(char ch, bool is_not_first_char)
{
	int letter_idx = 0;
	unsigned short morsecode_bits;

	if ('a' <= ch && ch <= 'z') {
		letter_idx = ch - 'a';
	} else if ('A' <= ch && ch <= 'Z') {
		letter_idx = ch - 'A';
	} else if (ch == ' ') {
		if (queue_lock()) {
			return -EFAULT;
		}
		kfifo_put(&flashed_codes_queue, SEPARATOR_SYMBOL);
		kfifo_put(&flashed_codes_queue, SEPARATOR_SYMBOL);
		queue_unlock();
		msleep((INTER_WORD_DOTTIMES - INTER_LETTER_DOTTIMES) * dottime);
		return 0;
	} else { // if invalid character
		return 0;
	}

	if (is_not_first_char) {
		if (queue_lock()) {
			return -EFAULT;
		}
		kfifo_put(&flashed_codes_queue, SEPARATOR_SYMBOL);
		queue_unlock();
		msleep((INTER_LETTER_DOTTIMES - 1) * dottime);
	}

	morsecode_bits = letter_to_morsecode_bits_map[letter_idx];
	if (flash_morsecode(morsecode_bits)) {
		return -EFAULT;
	}
	return 0;
}

static bool is_letter(char ch)
{
	return ('a' <= ch && ch <= 'z') || ('A' <= ch && ch <= 'Z');
}

static bool is_morsecode_char(char ch)
{
	return is_letter(ch) || (ch == ' ');
}

/******************************************************
 * File Operation Callbacks
 ******************************************************/

static ssize_t my_read(struct file *file,
                       char *buf, size_t count, loff_t *ppos)
{
	unsigned int bytes_copied = 0;

	if (queue_lock()) {
		return -EFAULT;
	}
	if (!kfifo_is_empty(&flashed_codes_queue)) {
		kfifo_put(&flashed_codes_queue, '\n');
	}
	if (kfifo_to_user(&flashed_codes_queue, buf, count, &bytes_copied)) {
		queue_unlock();
		return -EFAULT;
	}
	queue_unlock();

	*ppos += bytes_copied;
	return bytes_copied;
}

static ssize_t my_write(struct file *file,
                        const char *buff, size_t count, loff_t *ppos)
{
	int buff_idx;
	char ch;
	bool has_processed_first_char = false;

	// Trim leading spaces and invalid characters
	for (buff_idx = 0; buff_idx < count; ++buff_idx) {
		if (copy_from_user(&ch, &buff[buff_idx], sizeof(ch))) {
			return -EFAULT;
		}
		if (ch != ' ' && is_letter(ch)) {
			break;
		}
	}

	while (buff_idx < count) {
		if (copy_from_user(&ch, &buff[buff_idx], sizeof(ch))) {
			return -EFAULT;
		}
		if (ch == ' ') {
			int peek_buff_idx = buff_idx;
			char peek_ahead_ch;
			// Forward buffer index to character after spaces/invalid characters
			while (++peek_buff_idx < count) {
				if (copy_from_user(&peek_ahead_ch,
				                   &buff[peek_buff_idx],
				                   sizeof(peek_ahead_ch))) {
					return -EFAULT;
				}
				if (peek_ahead_ch != ' ' && is_letter(peek_ahead_ch)) {
					break;
				}
			}
			// Found spaces/invalid characters at end of string
			if (peek_buff_idx >= count) {
				break;
			}
			buff_idx = peek_buff_idx - 1; // set index to last space
		}
		if (is_morsecode_char(ch)) {
			if (process_morsecode(ch, has_processed_first_char)) {
				return -EFAULT;
			}
			has_processed_first_char = true;
		}
		buff_idx++;
	}
	*ppos += count;
	return count;
}

/******************************************************
 * Misc support
 ******************************************************/
// Callbacks:  (structure defined in /linux/fs.h)
struct file_operations my_fops = {
	.owner    =  THIS_MODULE,
	.read     =  my_read,
	.write    =  my_write,
};

// Character Device info for the Kernel:
static struct miscdevice my_miscdevice = {
	.minor    = MISC_DYNAMIC_MINOR,         // Let the system assign one.
	.name     = DEVICE_NAME,                // /dev/.... file.
	.fops     = &my_fops                    // Callback functions.
};

/******************************************************
 * Driver initialization and exit:
 ******************************************************/
static int __init my_init(void)
{
	int returnVal;

	driver_print(KERN_INFO, "Driver initialized.\n");
	INIT_KFIFO(flashed_codes_queue);

	// Validate dottime
	if (dottime < MIN_DOT_TIME || dottime > MAX_DOT_TIME) {
		dottime = DEFAULT_DOT_TIME;
		driver_print(KERN_WARNING,
		             "Invalid dottime given; valid range is [%d-%d]. Defaulting to %d.\n",
		             MIN_DOT_TIME,
		             MAX_DOT_TIME,
		             DEFAULT_DOT_TIME);
	}
	// Register as a misc driver
	returnVal = misc_register(&my_miscdevice);
	// Register new LED mode
	led_trigger_register_simple("morse-code", &led_trigger);
	return returnVal;
}

static void __exit my_exit(void)
{
	driver_print(KERN_INFO, "Driver exiting.\n");
	// Unregister LED mode
	led_trigger_unregister_simple(led_trigger);
	// Unregister misc driver
	misc_deregister(&my_miscdevice);
}

module_init(my_init);
module_exit(my_exit);

MODULE_AUTHOR("Karan Sharma, Mykhaylo Chavarha");
MODULE_DESCRIPTION("A driver that supports flashing Morse code on LEDs");
MODULE_LICENSE("GPL");
