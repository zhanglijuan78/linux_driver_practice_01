/**
 * A kernel module for controlling a GPIO LED/button pair. The device mounts devices via
 * sysfs /sys/class/gpio/gpio115 and gpio49. Therefore, this test LKM circuit assumes that an LED
 * is attached to GPIO 49 which is on P9_23 and the button is attached to GPIO 115 on P9_27. There
 * is no requirement for a custom overlay, as the pins are in their default mux mode states.
 */
 
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kobject.h>  // Using kobjects for the sysfs bindings
#include <linux/gpio.h>                 // Required for the GPIO functions
#include <linux/interrupt.h>            // Required for the IRQ code
#include <linux/time.h>

#define DEBOUNCE_TIME 200   // The default bounce time -- 200ms

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Derek Molloy");
MODULE_DESCRIPTION("A Button/LED test driver for the BBB");
MODULE_VERSION("0.1");

static bool isRising = 1;   // Rising edge is the default IRQ property
module_param(isRising, bool, S_IRUGO);    // Param desc. S_IRUGO can be read/not changed.
MODULE_PARM_DESC(isRising, " Rising edge = 1 (default), Falling edge = 0");  ///< parameter description

static unsigned int gpioLED = 24;
module_param(gpioLED, uint, S_IRUGO);     // Param desc. S_IRUGO can be read/not changed.
MODULE_PARM_DESC(gpioLED, " GPIO LED number (default=24)");  ///< parameter description

static unsigned int gpioButton = 22;
module_param(gpioButton, uint, S_IRUGO);     // Param desc. S_IRUGO can be read/not changed.
MODULE_PARM_DESC(gpioButton, " GPIO Button number (default=22)");  ///< parameter description

static char gpioName[8] = "gpioXXX";      ///< Null terminated default string -- just in case
static unsigned int irqNumber;
static unsigned int numberPresses = 0;
static bool ledOn = false;
static bool   isDebounce = 1;               ///< Use to store the debounce state (on by default)
static struct timespec ts_last, ts_current, ts_diff;  ///< timespecs from linux/time.h (has nano precision)

// Function prototype for the custom IRQ handler function -- see below for the implementation
static irq_handler_t ebbgpio_irq_handler(unsigned int irq, void *dev_id,
                                         struct pt_regs *regs);

/** @brief A callback function to output the numberPresses variable
 *  @param kobj represents a kernel object device that appears in the sysfs filesystem
 *  @param attr the pointer to the kobj_attribute struct
 *  @param buf the buffer to which to write the number of presses
 *  @return return the total number of characters written to the buffer (excluding null)
 */
static ssize_t numberPresses_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
  return sprintf(buf, "%d\n", numberPresses);
}

/** @brief A callback function to read in the numberPresses variable
 *  @param kobj represents a kernel object device that appears in the sysfs filesystem
 *  @param attr the pointer to the kobj_attribute struct
 *  @param buf the buffer from which to read the number of presses (e.g., reset to 0).
 *  @param count the number characters in the buffer
 *  @return return should return the total number of characters used from the buffer
 */
static ssize_t numberPresses_store(struct kobject *kobj, struct kobj_attribute *attr,
    const char *buf, size_t count) {
  sscanf(buf, "%du", &numberPresses);
  return count;
}

// If the LED is on or off
static ssize_t ledOn_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf) {
  return sprintf(buf, "%d\n", ledOn);
}

/** @brief Displays the last time the button was pressed -- manually output the date (no localization) */
static ssize_t lastTime_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf){
     return sprintf(buf, "%.2lu:%.2lu:%.2lu:%.9lu \n", (ts_last.tv_sec/3600)%24,
                   (ts_last.tv_sec/60) % 60, ts_last.tv_sec % 60, ts_last.tv_nsec );
}

/** @brief Display the time difference in the form secs.nanosecs to 9 places */
static ssize_t diffTime_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf){
     return sprintf(buf, "%lu.%.9lu\n", ts_diff.tv_sec, ts_diff.tv_nsec);
}

/** @brief Displays if button debouncing is on or off */
static ssize_t isDebounce_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf){
     return sprintf(buf, "%d\n", isDebounce);
}

// Stores and sets the debounce state
static ssize_t isDebounce_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf,
    size_t count) {
  unsigned int temp;
  sscanf(buf, "%du", &temp);
  // Debounce off.
  gpio_set_debounce(gpioButton, 0);
  isDebounce = temp;
  if (isDebounce) {
    gpio_set_debounce(gpioButton, DEBOUNCE_TIME);
    printk(KERN_INFO "EBB Button: Debounce on\n");
  } else {
    // Debounce off.
    gpio_set_debounce(gpioButton, 0);
    printk(KERN_INFO "EBB Button: Debounce off\n");
  }
  return count;
}

/**  Use these helper macros to define the name and access levels of the kobj_attributes
 *  The kobj_attribute has an attribute attr (name and mode), show and store function pointers
 *  The count variable is associated with the numberPresses variable and it is to be exposed
 *  with mode 0666 using the numberPresses_show and numberPresses_store functions above
 */
// Warning! Need write-all permission so overriding check defined in kernel.h
#undef VERIFY_OCTAL_PERMISSIONS
#define VERIFY_OCTAL_PERMISSIONS(perms) (perms)
static struct kobj_attribute count_attr = __ATTR(numberPresses, 0666, numberPresses_show, numberPresses_store);
static struct kobj_attribute debounce_attr = __ATTR(isDebounce, 0666, isDebounce_show, isDebounce_store);

/**  The __ATTR_RO macro defines a read-only attribute. There is no need to identify that the
 *  function is called _show, but it must be present. __ATTR_WO can be  used for a write-only
 *  attribute but only in Linux 3.11.x on.
 */
static struct kobj_attribute ledon_attr = __ATTR_RO(ledOn);     ///< the ledon kobject attr
static struct kobj_attribute time_attr  = __ATTR_RO(lastTime);  ///< the last time pressed kobject attr
static struct kobj_attribute diff_attr  = __ATTR_RO(diffTime);  ///< the difference in time attr


/**  The ebb_attrs[] is an array of attributes that is used to create the attribute group below.
 *  The attr property of the kobj_attribute is used to extract the attribute struct
 */
static struct attribute *ebb_attrs[] = {
  &count_attr.attr,                  ///< The number of button presses
  &ledon_attr.attr,                  ///< Is the LED on or off?
  &time_attr.attr,                   ///< Time of the last button press in HH:MM:SS:NNNNNNNNN
  &diff_attr.attr,                   ///< The difference in time between the last two presses
  &debounce_attr.attr,               ///< Is the debounce state true or false
  NULL,
};

/**  The attribute group uses the attribute array and a name, which is exposed on sysfs -- in this
 *  case it is gpio115, which is automatically defined in the ebbButton_init() function below
 *  using the custom kernel parameter that can be passed when the module is loaded.
 */
static struct attribute_group attr_group = {
  .name  = gpioName,                 ///< The name is generated in ebbButton_init()
  .attrs = ebb_attrs,                ///< The attributes array defined just above
};

static struct kobject *ebb_kobj;

/** @brief The LKM initialization function
 *  The static keyword restricts the visibility of the function to within this C file. The __init
 *  macro means that for a built-in driver (not a LKM) the function is only used at initialization
 *  time and that it can be discarded and its memory freed up after that point. In this example this
 *  function sets up the GPIOs and the IRQ
 *  @return returns 0 if successful
 */
static int __init ebbgpio_init(void) {
  int result = 0;
  unsigned long IRQflags = IRQF_TRIGGER_RISING;   // Interrupt on rising edge (button press, not release)

  printk(KERN_INFO "GPIO_TEST: Initializing the GPIO_TEST LKM\n");
  sprintf(gpioName, "gpio%d", gpioButton);  // Create the gpio22 name for /sys/ebb/gpio22

  // create the kobject sysfs entry at /sys/ebb -- probably not an ideal location!
  ebb_kobj = kobject_create_and_add("ebb", kernel_kobj->parent); // kernel_kobj points to /sys/kernel
  if(!ebb_kobj) {
    printk(KERN_ALERT "EBB Button: failed to create kobject mapping\n");
    return -ENOMEM;
  }
     
  // add the attributes to /sys/ebb/ -- for example, /sys/ebb/gpio115/numberPresses
  result = sysfs_create_group(ebb_kobj, &attr_group);
  if(result) {
    printk(KERN_ALERT "EBB Button: failed to create sysfs group\n");
    kobject_put(ebb_kobj);                          // clean up -- remove the kobject sysfs entry
    return result;
  }

  getnstimeofday(&ts_last);                          // set the last time to be the current time
  ts_diff = timespec_sub(ts_last, ts_last);          // set the initial time difference to be 0
 
  // Is the GPIO a valid GPIO number?
  if (!gpio_is_valid(gpioLED)) {
    printk(KERN_INFO "GPIO_TEST: invalid LED GPIO\n");
    return -ENODEV;
  }

  // Going to set up the LED. It is a GPIO in output mode and will be on by default.
  ledOn = true;
  gpio_request(gpioLED, "sysfs");  // request the GPIO
  gpio_direction_output(gpioLED, ledOn);  // set the GPIO in OUTPUT mode and on
  gpio_export(gpioLED, false);  // Causes gpio49 to appear in /sys/class/gpio. The bool argument prevents the direction from being changed

  gpio_request(gpioButton, "sysfs");  // request the GPIO
  gpio_direction_input(gpioButton);  // set the GPIO in INPUT mode
  gpio_set_debounce(gpioButton, DEBOUNCE_TIME);  // Debounce the button GPIO with a delay of 200ms
  gpio_export(gpioLED, false);  // Causes gpio49 to appear in /sys/class/gpio. The bool argument prevents the direction from being changed

  // Perform a quick test to see that the button is working as expected on LKM load
  printk(KERN_INFO "GPIO_TEST: The button state is currently: %d\n", gpio_get_value(gpioButton));

  // GPIO numbers and IRQ numbers are not the same! This function performs the mapping for us
  irqNumber = gpio_to_irq(gpioButton);
  printk(KERN_INFO "GPIO_TEST: The button is mapped to IRQ: %d\n", irqNumber);

  if(!isRising){                           // If the kernel parameter isRising=0 is supplied
    IRQflags = IRQF_TRIGGER_FALLING;      // Set the interrupt to be on the falling edge. Trigger the interrupt when button is released.
  }

  // This next call requests an interrupt line
  result = request_irq(irqNumber,             // The interrupt number requested
                       (irq_handler_t) ebbgpio_irq_handler, // The pointer to the handler function below
                       IRQflags,   // Interrupt on rising edge (button press, not release)
                       "ebb_gpio_handler",    // Used in /proc/interrupts to identify the owner
                       NULL);                 // The *dev_id for shared interrupt lines, NULL is okay

  printk(KERN_INFO "GPIO_TEST: The interrupt request result is: %d\n", result);
  return result;
}

/** @brief The LKM cleanup function
 *  *  Similar to the initialization function, it is static. The __exit macro notifies that if this
 *   *  code is used for a built-in driver (not a LKM) that this function is not required. Used to release the
 *    *  GPIOs and display cleanup messages.
 *     */
static void __exit ebbgpio_exit(void){
  printk(KERN_INFO "EBB Button: The button was pressed %d times\n", numberPresses);
  kobject_put(ebb_kobj);                   // clean up -- remove the kobject sysfs entry
  gpio_set_value(gpioLED, 0);              // Turn the LED off, makes it clear the device was unloaded
  gpio_unexport(gpioLED);                  // Unexport the LED GPIO
  free_irq(irqNumber, NULL);               // Free the IRQ number, no *dev_id required in this case
  gpio_unexport(gpioButton);               // Unexport the Button GPIO
  gpio_free(gpioLED);                      // Free the LED GPIO
  gpio_free(gpioButton);                   // Free the Button GPIO
  printk(KERN_INFO "EBB Button: Goodbye from the EBB Button LKM!\n");
}

static irq_handler_t ebbgpio_irq_handler(unsigned int irq, void *dev_id,
                                         struct pt_regs *regs) {
  ledOn = !ledOn;                      // Invert the LED state on each button press
  gpio_set_value(gpioLED, ledOn);      // Set the physical LED accordingly
  getnstimeofday(&ts_current);         // Get the current time as ts_current
  ts_diff = timespec_sub(ts_current, ts_last);   // Determine the time difference between last 2 presses
  ts_last = ts_current;                // Store the current time as the last time ts_last
  printk(KERN_INFO "EBB Button: The button state is currently: %d\n", gpio_get_value(gpioButton));
  numberPresses++;                     // Global counter, will be outputted when the module is unloaded
  return (irq_handler_t) IRQ_HANDLED;  // Announce that the IRQ has been handled correctly
}

// This next calls are  mandatory -- they identify the initialization function
// and the cleanup function (as above).
module_init(ebbgpio_init);
module_exit(ebbgpio_exit);
