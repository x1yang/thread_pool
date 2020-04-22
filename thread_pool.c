/**********************************************************************/

#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<unistd.h>
#include<errno.h>
#include<pthread.h>
#include<sys/socket.h>
#include<arpa/inet.h>
#include<signal.h>
#include<time.h>

#define DEFAULT_TIME 1 //1秒检测一次
#define MIN_WAIT_TASK_NUM 10//如果queue_size > MIN_WAIT_TASK_NUM 添加新的线程到线程池
#define DEFAULT_THREAD_VARY 10//每次创建和销毁线程的个数
#define true 1
#define false 0

void sys_err(const char* str)
{
	perror(str);
	exit(1);	
}

typedef struct
{
	void *(*function)(void*);         //函数指针
	void *arg;                        //上面函数的参数
}threadpool_task_t;                   //各线程任务结构体

//描述线程池相关信息结构体
typedef struct
{
	pthread_mutex_t lock;             //用于锁住本结构体
	pthread_mutex_t thread_counter;   //用于锁住忙线程数
	
	pthread_cond_t queue_not_full;    //当任务队列满时，添加任务的线程阻塞，等待此条件变量满足
	pthread_cond_t queue_not_empty;   //当任务队列不为空时，通知等待处理任务的线程

	pthread_t threads;               //线程池中临时存放每个线程的tid
	pthread_t adjust_tid;             //存放管理线程的tid
	threadpool_task_t *task_queue;    //任务队列

	int min_thr_num;                  //线程池最小线程数
	int max_thr_num;                  //线程池最大线程数
	int live_thr_num;                 //存活线程数
	int busy_thr_num;                 //忙线程数
	int wait_exit_thr_num;            //将要销毁的线程数

	int queue_front;                  //task_queeue队头下标
	int queue_rear;                   //task_queeue队尾下标
	int queue_size;                   //task_queeue队中实际任务数
	int queue_max_size;               //task_queeue队中可容纳任务数上线
	int shutdown;                     //标志位，线程池使用状态，true或fals

}threadpool_t;

//创建、初始化线程池
threadpool_t *threadpool_create(int min_thr_num, int max_thr_num, int queue_max_size);

//线程池线程的主函数
void* threadpool_thread(void *threadpool);

//管理者线程的主函数
void *adjust_thread(void *threadpool);

//判断线程是否存活
int is_thread_alive(pthread_t tid);

//向线程池中添加一个任务
int threadpool_add(threadpool_t *pool, void *(*function)(void *arg), void* arg);

//数据处理的方法
void *process(void *arg);

//销毁线程池
int threadpool_destory(threadpool_t *pool);

//释放线程池空间
int threadpool_free(threadpool_t *pool);

//工作线程的回调函数
void* threadpool_thread(void *threadpool)
{
	threadpool_t *pool = (threadpool_t*)threadpool;
	threadpool_task_t task;
	while(1)
	{
		//刚创建出线程，等待任务队列里有任务，否则阻塞，待有任务出现后再唤醒处理任务
		pthread_mutex_lock(&(pool->lock));
		
		//queue_size == 0 说明没有任务，调用wait阻塞在条件变量上，若有任务跳过该while循环
		while((pool->queue_size == 0)&&(!pool->shutdown))
		{
			printf("线程号为 0x%x 的线程正在阻塞等待任务......\n", (unsigned int)pthread_self());
			pthread_cond_wait(&(pool->queue_not_empty), &(pool->lock));
			printf("解除阻塞\n");
			//清除指定数目的空闲线程如果要结束的线程个数大于0，则结束线程
			if(pool->wait_exit_thr_num > 0)
			{
				pool->wait_exit_thr_num--;
				//如果线程池里线程个数大于最小值时可以结束当前线程
				if(pool->live_thr_num > pool->min_thr_num)
				{
					printf("-KO-线程号为 0x%x 的线程被销毁\n", (unsigned int)pthread_self());
					pool->live_thr_num--;
					pthread_mutex_unlock(&(pool->lock));

					pthread_exit(NULL);
				}
			}
		}

  //如果指定了true，要关闭线程池里的每个线程，自行退出处理
		if (pool->shutdown) 
		{
			printf("-----------------------------------------------shutdown==true\n");
			printf("-KO-线程号为 0x%x 的线程被销毁\n", (unsigned int)pthread_self());
			pool->live_thr_num--;
	    	pthread_mutex_unlock(&(pool->lock));
			pthread_exit(NULL);     /* 线程自行结束 */
		}

		//从队列里获取任务，是一个出队操作
		task.function = pool->task_queue[pool->queue_front].function;
		task.arg = pool->task_queue[pool->queue_front].arg;

		pool->queue_front = (pool->queue_front + 1) % pool->queue_max_size;//出队，模拟环形队列
		pool->queue_size--;

		//通知可以有新的任务添加进来
		pthread_cond_broadcast(&(pool->queue_not_full));

		//任务出队以后，立即将线程池释放
		pthread_mutex_unlock(&(pool->lock));

		//执行任务
		printf("+++线程号为 0x%x 的线程开始工作\n",(unsigned int)pthread_self());
		pthread_mutex_lock(&(pool->thread_counter));//忙线程数变量锁
		pool->busy_thr_num++;//忙线程数+1
		pthread_mutex_unlock(&(pool->thread_counter));

		task.function(task.arg);//执行回调函数
		//(*(task.function))(task.arg);

		//任务处理结束
		printf("--------线程号为 0x%x 的线程工作结束\n",(unsigned int)pthread_self());
		pthread_mutex_lock(&(pool->thread_counter));
		pool->busy_thr_num--;//处理掉一个任务，忙线程数-1
		pthread_mutex_unlock(&(pool->thread_counter));
	}
	pthread_exit(NULL);
	
}

//创建线程
threadpool_t *threadpool_create(int min_thr_num, int max_thr_num, int queue_max_size)
{
	int i;
	threadpool_t *pool = NULL;//线程池结构体
	do
	{
		if((pool = (threadpool_t *)malloc(sizeof(threadpool_t))) == NULL)
		{
			printf("malloc threadpool fail\n"); 
			break;
		}

		pool->min_thr_num = min_thr_num;
		pool->max_thr_num = max_thr_num;
		pool->busy_thr_num = 0;
		pool->live_thr_num = min_thr_num;//活着的线程数初值=最小线程数
		pool->wait_exit_thr_num = 0;

		pool->queue_size = 0;//实际任务数为0
		pool->queue_max_size = queue_max_size;//赋值最大任务队列数
		pool->queue_front = 0;
		pool->queue_rear = 0;
		pool->shutdown = false;//不关闭线程池

		//给任务队列开辟空间
		pool->task_queue = (threadpool_task_t*)malloc(sizeof(threadpool_task_t) * queue_max_size);
		if(pool->task_queue == NULL)
		{
			printf("malloc task_queue fail\n");
			break;
		}
		memset(pool->task_queue, 0 ,sizeof(threadpool_task_t) * queue_max_size);

		//初始化互斥锁、条件变量
		if(pthread_mutex_init(&(pool->lock), NULL) !=0 || pthread_mutex_init(&(pool->thread_counter),NULL) != 0 || pthread_cond_init(&(pool->queue_not_empty
			), NULL) != 0 || pthread_cond_init(&(pool->queue_not_full),NULL) != 0)
		{
			printf("init the lock or cond fail\n");
			break;
		}

		//创建 min_thr_num 个线程
		for(i = 0; i < min_thr_num; i++)
		{
			pthread_create(&(pool->threads), NULL, threadpool_thread, (void*)pool);//pool指向当前线程池
			pthread_detach(pool->threads);
			printf("～～线程号为 0x%x 的线程被创建\n",(unsigned int)pool->threads);
		}
		pthread_create(&(pool->adjust_tid), NULL, adjust_thread, (void*)pool);//创建管理线程
		return pool;

	}
	while(0);
	threadpool_free(pool);//前面出现错误时，释放pool空间
	return NULL;
	
}

//管理者线程
void *adjust_thread(void *threadpool)
{
	int i;
	threadpool_t *pool = (threadpool_t*)threadpool;
	while(!pool->shutdown)	
	{
		sleep(DEFAULT_TIME);//定时对线程池进行管理
		
		pthread_mutex_lock(&(pool->lock));
		int queue_size = pool->queue_size;//关注任务数
		int live_thr_num = pool->live_thr_num;//关注存活线程数
		pthread_mutex_unlock(&(pool->lock));

		pthread_mutex_lock(&(pool->thread_counter));
		int busy_thr_num = pool->busy_thr_num;//忙线程数
		pthread_mutex_unlock(&(pool->thread_counter));

		//创建新线程算法：任务数大于最小任务个数，且存活的线程数少于最大线程个数时
		if(queue_size >= MIN_WAIT_TASK_NUM && live_thr_num < pool->max_thr_num)
		{
			pthread_mutex_lock(&(pool->lock));
			
			//一次增加DEFAULT_THREAD_VARY个线程
			for(i = 0; i < DEFAULT_THREAD_VARY && pool->live_thr_num < pool->max_thr_num; i++)
			{
				pthread_create(&(pool->threads), NULL, threadpool_thread, (void*)pool);
				pthread_detach(pool->threads);
				pool->live_thr_num++;
			}
			printf("----------------------扩容---------------------存活线程数为：%d\n", pool->live_thr_num);

			pthread_mutex_unlock(&(pool->lock));
		}

		//销毁多余的空闲线程 算法：忙线程x2 小于存活的线程数且存活的线程数大于最小线程数时
		if((busy_thr_num * 2) < live_thr_num && live_thr_num > pool->min_thr_num)
		{
			//一次性销毁DEFAULT_THREAD个线程，随机10个即可
			pthread_mutex_lock(&(pool->lock));
			pool->wait_exit_thr_num = DEFAULT_THREAD_VARY;//要销毁的线程数设置为10
			pthread_mutex_unlock(&(pool->lock));

			//pthread_cond_broadcast(&(pool->queue_not_empty));

			for(i = 0; i< DEFAULT_THREAD_VARY; i++)
			{
				//通知处在空闲状态的线程，他们会自行终止
				pthread_cond_signal(&(pool->queue_not_empty));
			}
			sleep(1);//为了调试
			printf("----------------------缩容---------------------存活线程数为：%d\n", pool->live_thr_num);
			
		}

	}
	
	return NULL;
	
}

//向线程池中添加一个任务
int threadpool_add(threadpool_t *pool, void *(*function)(void *arg), void* arg)
{
	pthread_mutex_lock(&(pool->lock));
	
	//为真，队列已满，调用wait阻塞
	while((pool->queue_size == pool->queue_max_size) && (!pool->shutdown))
	{
		pthread_cond_wait(&(pool->queue_not_full), &(pool->lock));
	}

	if(pool->shutdown)
	{
		pthread_cond_broadcast(&(pool->queue_not_empty));
		pthread_mutex_unlock(&(pool->lock));
		return 0;
	}
	//清空工作线程调用的回调函数的参数arg
	if(pool->task_queue[pool->queue_rear].arg != NULL)
	{
		pool->task_queue[pool->queue_rear].arg = NULL;
	}

	//添加任务到任务队列
	pool->task_queue[pool->queue_rear].function = function;
	pool->task_queue[pool->queue_rear].arg = arg;
	pool->queue_rear = (pool->queue_rear + 1) % pool->queue_max_size;//队尾指针移动模拟环形队列
	pool->queue_size++;
	
	//添加完任务后，队列不为空，唤醒线程池中等待处理任务的线程
	pthread_cond_signal(&(pool->queue_not_empty));
	pthread_mutex_unlock(&(pool->lock));

	return 0;
}

//销毁线程池
int threadpool_destory(threadpool_t *pool)
{
	if(pool == NULL)
	{
		return -1;
	}
	pool->shutdown = true;

	//先销毁线管理线程
	pthread_join(pool->adjust_tid, NULL);

	/*while(pool->busy_thr_num)
	{
		pthread_cond_broadcast(&(pool->queue_not_empty));	
	}*/
	pthread_cond_broadcast(&(pool->queue_not_empty));	
	printf("----------------------线程池销毁即将完毕\n");
	while(1)
	{
		pthread_mutex_lock(&(pool->lock));
		if(pool->live_thr_num == 0)
		{
			pthread_mutex_unlock(&(pool->lock));
			break;
		}
		else
		{
			printf("正在销毁线程池，请稍等...\n");
			pthread_mutex_unlock(&(pool->lock));
			sleep(1);
		}
	}
	printf("线程池销毁完毕，即将释放内存空间\n");
	threadpool_free(pool);
	return 0;
}

int threadpool_free(threadpool_t *pool)
{
	if(pool == NULL)
	{
		return -1;
	}
	if(pool->task_queue)
	{
		free(pool->task_queue);
	}
	pthread_mutex_lock(&(pool->lock));
	pthread_mutex_destroy(&(pool->lock));
	pthread_mutex_lock(&(pool->thread_counter));
	pthread_mutex_destroy(&(pool->thread_counter));
	pthread_cond_destroy(&(pool->queue_not_empty));
	pthread_cond_destroy(&(pool->queue_not_full));
	free(pool);
	pool = NULL;
	printf("内存空间释放完毕\n");
	return 0;
}
//废弃
#if 0
int is_thread_alive(pthread_t tid)
{
	int kill_rc = pthread_kill(tid, 0);     //发0号信号，测试线程是否存活,线程不存在可能会产生段错误。
	if (kill_rc == ESRCH) 
	{
		return 0;
	}
	return 1;
}
#endif

void *process(void *arg)
{
	printf("...线程号为 0x%x 的线程正在处理数据 %d\n ",(unsigned int)pthread_self(),*(int *)arg);
	usleep((rand()%50+1)*1000);//模拟处理任务
	printf("数据 %d 处理完毕\n",*(int *)arg);

	return NULL;
}


int main(int argc, char* argv[])
{
	srand(time(NULL));
	/*threadpool_t *threadpool_create(int min_thr_num, int max_thr_num, int queue_max_size)*/
	threadpool_t *thp = threadpool_create(5,100,100);//创建线程池，线程池中最小5个线程，最大100个线程，队列最大100
	printf("pool inited\n");
	
	int num[2048];
	int i;
	for(i = 0; i < 2048; i++)
	{
		if(i % 500 == 100)
		{
			sleep(1);
		}
		num[i] = i;
		printf("><生产出数据 %d\n", i);
		threadpool_add(thp, process, (void*)&num[i]);//想线程中添加任务
	}
	sleep(20);//等待子线程完成任务
	printf("----------------存活线程数为：%d\n", thp->live_thr_num);
	printf("----------------即将销毁线程池---------------------\n");
	threadpool_destory(thp);
	sleep(20);
	pthread_exit(NULL);
}

/***********************************************************************/
/*********************************end***********************************/
/***********************************************************************/

