/*
 * my_printk.h
 *
 *  Created on: 2016年9月9日
 *      Author: zhuce
 */

#ifndef MY_PRINTK_H_
#define MY_PRINTK_H_

#define MSG_OUT

#ifdef MSG_OUT
#define my_printk(s, ...) printk(s, ##__VA_ARGS__)
#else
#define my_printk(s, ...)
#endif

#endif /* MY_PRINTK_H_ */
