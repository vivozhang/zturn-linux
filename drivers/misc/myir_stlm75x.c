#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/device.h>
//#include <linux/sysdev.h>
#include <asm/uaccess.h>
#include <linux/slab.h>
#include <linux/kdev_t.h>
#include <linux/i2c.h>

#define DEV_NAME		"myir-stlm75x"

/*********************
 *	Register define  *
 *********************/
#define REG_TEMP	0x0
#define REG_CON		0x1
#define REG_TOS		0x2
#define REG_THYS	0x3

#define MAX_CONV_MS	150
#define SIGN_MASK	(0x1 << 15)
#define TEMP_SHIFT	7
#define TEMP_MASK	(0xFF << TEMP_SHIFT)
#define DEGREE_PER_CNT	0.5

struct myir_stlm75x_data {
	struct i2c_client *client;
	struct class class;
	struct mutex mutex;
	u16 temp_tos;
	u16 temp_thys;
	u16 temp_value;
};

inline int to_readable_value(u16 _value)
{
	int value;
	if (_value & SIGN_MASK) {
		value = -(~((_value & TEMP_MASK) >> TEMP_SHIFT) + 1);
	} else {
		value = ((_value & TEMP_MASK) >> TEMP_SHIFT);
	}
//	printk(KERN_ERR "value to read: %d\n", value);
	return value;
}

#if 0
inline u16 to_register_value(int/*float*/ degreex2)
{
	u16 ret;
	if (degreex2 > 255) {
		degreex2 = 255;
	} else if (degreex2 < -255) {
		degreex2 = -255;
	}
	ret = (u16)degreex2;
	if (degreex2 >= 0) {
		ret = (ret << TEMP_SHIFT) & TEMP_MASK;
	} else {
		ret = (ret << TEMP_SHIFT) & 0xFFFF;
	}
	printk(KERN_ERR "value to write: %#X\n", ret);
	return ret;
}
#endif

static int myir_stlm75x_readwrite(struct i2c_client *client,
                   u16 wr_len, u8 *wr_buf,
                   u16 rd_len, u8 *rd_buf)
{
    struct i2c_msg wrmsg[2];
    int i = 0;
    int ret;

    if (wr_len) {
        wrmsg[i].addr  = client->addr;
        wrmsg[i].flags = 0;
        wrmsg[i].len = wr_len;
        wrmsg[i].buf = wr_buf;
        i++;
    }
    if (rd_len) {
        wrmsg[i].addr  = client->addr;
        wrmsg[i].flags = I2C_M_RD;
        wrmsg[i].len = rd_len;
        wrmsg[i].buf = rd_buf;
        i++;
    }

    ret = i2c_transfer(client->adapter, wrmsg, i);
    if (ret < 0)
        return ret;
    if (ret != i)
        return -EIO;

    return 0;
}

static int myir_stlm75x_write_word(struct myir_stlm75x_data *pdata,
                     u8 addr, u16 value)
{
    u8 wrbuf[3]={0};

    wrbuf[0] = addr;
    wrbuf[1] = (value>>8)&0xFF;
    wrbuf[2] = value&0xFF;

    return myir_stlm75x_readwrite(pdata->client, 3, wrbuf, 0, NULL);
}

static int myir_stlm75x_write_byte(struct myir_stlm75x_data *pdata,
                     u8 addr, u8 value)
{
    u8 wrbuf[2]={0};

    wrbuf[0] = addr;
    wrbuf[1] = value;

    return myir_stlm75x_readwrite(pdata->client, 2, wrbuf, 0, NULL);
}

static int myir_stlm75x_read_word(struct myir_stlm75x_data *pdata,
                    u8 addr)
{
    u8 wrbuf[2], rdbuf[2]={0};
    int error;

    wrbuf[0] = addr;

    error = myir_stlm75x_readwrite(pdata->client, 1, wrbuf, 2, rdbuf);
    if (error)
        return error;

    return rdbuf[0]<<8|rdbuf[1];
}

static int myir_stlm75x_read_byte(struct myir_stlm75x_data *pdata,
                    u8 addr)
{
    u8 wrbuf[2], rdbuf[2]={0};
    int error;

    wrbuf[0] = addr;

    error = myir_stlm75x_readwrite(pdata->client, 1, wrbuf, 1, rdbuf);
    if (error)
        return error;

    return rdbuf[0];
}

/* class attribute show function. */
static ssize_t myir_stlm75x_show(struct class *cls, struct class_attribute *attr, char *buf)
{
	struct myir_stlm75x_data *pdata = (struct myir_stlm75x_data *)container_of(cls, struct myir_stlm75x_data, class);
	int ret;
	int value;
	unsigned long start_time;
	
	mutex_lock(&pdata->mutex);
	
	start_time = jiffies;
	pdata->temp_value = myir_stlm75x_read_word(pdata, REG_TEMP);
//	printk(KERN_ERR "pdata->temp_value: %#X", pdata->temp_value);
	value = to_readable_value(pdata->temp_value);
	ret = sprintf(buf, "%d.%s\n", value/2, value%2?"5":"0");

	while (time_before(jiffies, start_time + msecs_to_jiffies(MAX_CONV_MS))) schedule();
	
	mutex_unlock(&pdata->mutex);
	
	return ret;
}

/* Attributes declaration: Here I have declared only one attribute attr1 */
static struct class_attribute myir_stlm75x_class_attrs[] = {
	__ATTR(value_degree, S_IRUGO | S_IWUSR , myir_stlm75x_show, NULL), //use macro for permission
	__ATTR_NULL
};

static int myir_stlm75x_probe(struct i2c_client *client,
                     const struct i2c_device_id *id)
{
	int ret = 0;
	struct myir_stlm75x_data *pdata = NULL;
	
	printk(KERN_ALERT "%s()\n", __func__);
	
	pdata = kmalloc(sizeof(struct myir_stlm75x_data), GFP_KERNEL);
	if(!pdata) {
		printk(KERN_ERR "No memory!\n");
		return -ENOMEM;
	}
	memset(pdata, 0, sizeof(struct myir_stlm75x_data));

	pdata->client = client;
	
	/* Init class */
	mutex_init(&pdata->mutex);
	pdata->class.name = DEV_NAME;
	pdata->class.owner = THIS_MODULE;
	pdata->class.class_attrs = myir_stlm75x_class_attrs;
	ret = class_register(&pdata->class);
	if(ret) {
		printk(KERN_ERR "class_register failed!\n");
		goto class_register_fail;
	}
	i2c_set_clientdata(client, pdata);
	
	printk(KERN_ALERT "%s driver initialized successfully!\n", DEV_NAME);
	return 0;

class_register_fail:
	
	return ret;
}

static int myir_stlm75x_remove(struct i2c_client *client)
{
    struct myir_stlm75x_data *pdata = i2c_get_clientdata(client);
	
	class_unregister(&pdata->class);
	kfree(pdata);
	i2c_set_clientdata(client, NULL);
    return 0;
}

static const struct i2c_device_id myir_stlm75x_id[] = {
    { DEV_NAME, 0 },
    { }
};

static struct i2c_driver myir_stlm75x_driver = {
    .driver = {
        .owner	= THIS_MODULE,
        .name	= DEV_NAME,
    },
    .id_table	= myir_stlm75x_id,
    .probe		= myir_stlm75x_probe,
    .remove		= myir_stlm75x_remove,
};

module_i2c_driver(myir_stlm75x_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Kevin Su <kevin.su@myirtech.com>");
MODULE_DESCRIPTION("MYIR stlm75x temperature driver.");
