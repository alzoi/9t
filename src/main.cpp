/*
HTTP-сервер:
1) принимает опции -h <IP адрес>, -p <порт>, -d <домашняя папка web-сервера>;
2) создаёт процессы испольнители GET запросов (воркеры) от пользователей;
3) параллельно отправляет файлы пользователям;
4) для организации ассинхронного опроса событий на дескрипторах файлов использует библиотеку libev.

Установка библиотек cmake и libev (http://dist.schmorp.de/libev/):
apt-get install cmake libev4 libev-dev

Сборка:
g++ main.cpp -std=c++11 -lev -lpthread -o final
*/
#include <iostream>
#include <map>
#include <list>
#include <semaphore.h>
#include <sys/types.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <syslog.h>
#include <semaphore.h>
// Библиотека libev.
#include <ev.h>

// Имя проекта.
#define _NAME_PROJ "final"

// Для вывода отладочных сообщений.
//#define _R_DEBUG
//
#ifdef _R_DEBUG
   #define PDEBUG(...) printf(__VA_ARGS__)
#else
   #define PDEBUG(...)  
#endif

// Число воркеров (параллельных исполнителей GET запросов пользователей).
#define _COUNT_WORKERS 3

// Переменные:
// Указатель на семафор, для блокировки одновременного доступа к списку пользовательских сокетов.
sem_t* locker;
// Опции заданные в командной строке.
char *host = 0, *port = 0, *dir = 0;
// Отсортированный ассоциативный контейнер - который хранит sp[0] дескриптор пары-сокетов,
// который соединён с конкретным воркером, при TRUE - воркер свободен от задач.
std::map<int, bool> gt_workers;
// Список дескрипторов подключившихся к серверу slave-сокетов, список выполнен в виде очереди
//(вначале первые подключившиеся, вконце последние).
std::list<int> gt_ready_read_sockets;

// Функции:
// Создание воркеров.
pid_t create_worker(void);
// Перевод сокета в неблокирующий режим.
int set_nonblock(int fd);
// Получение и передача дескриптора файла.
ssize_t sock_fd_read(int sock, void *buf, ssize_t bufsize, int *fd);
ssize_t sock_fd_write(int sock, void *buf, ssize_t buflen, int fd);
void extract_path_from_http_get_request(std::string& path, const char* buf, ssize_t len);
void process_slave_socket(int slave_socket);

// CALLBACK:
void master_accept_connection(struct ev_loop *loop, struct ev_io *w, int revents);
void do_work(struct ev_loop *loop, struct ev_io *w, int revents);
void set_worker_free(struct ev_loop *loop, struct ev_io *w, int revents);
void slave_send_to_worker(struct ev_loop *loop, struct ev_io *w, int revents);

//
int start_daemon(){
// Функция превращает текущий процесс в демон (потомок init, без доступа к терминалу).
   pid_t pid;
   int i;
   // Создаём дочерний поцесс.
   pid = fork();
   if(pid < 0) {
      // Выход если ошибка.
      exit(EXIT_FAILURE);
   } else if (pid > 0) {
      // Закрываем родительский процесс.
      exit(EXIT_SUCCESS);
   }
// Продолжаем работать в дочернем процессе (№ 1).
   // Создаём новый сеанс и группу процессов
   //(текущий дочерний процесс становится главным процессом в сеансе и группе процессов),
   // не имеет управляющего терминала.
   if(setsid() < 0) {
      exit(EXIT_FAILURE);
   }
   // Игнорируем сигналы SIGHUP - посылает главный процесс всем процессам сеанса при завершении.
   signal(SIGHUP, SIG_IGN);
   // Игнорируем сигналы SIGCHLD - приходит когда дочерний процесс завершается.
   signal(SIGCHLD, SIG_IGN);
   // Второй раз создаём дочерний процесс (закрывая текущий), чтобы гарантировать, что демон не сможет
   // автоматически получить управляющий терминал.
   pid = fork();
   if(pid < 0) {
      exit(EXIT_SUCCESS);      
   } else if (pid > 0) {
      // Закрываем родительский процесс.
      exit(EXIT_SUCCESS);      
   }
// Продолжаем работать в дочернем процессе (№ 2).
   // Разрешаем выставлять все биты прав на создаваемые в процессе файлы.
   umask(0);
   // Установка корневого каталога.
   if(chdir("/") < 0){
      exit(EXIT_FAILURE);
   }
   // Закрываем все открытые файлы.
   for(i = sysconf(_SC_OPEN_MAX); i > 0; i--){
      close(i);
   }
#ifndef _R_DEBUG
   // Открываем файлы 0, 1, 2 и перенаправляем их в /dev/null
   open("/dev/null", O_RDWR);
   dup(0);
   dup(0);
#endif
   // Открываем файл журнала (в терминал мы писать не можем).
   openlog(_NAME_PROJ, LOG_PID, LOG_DAEMON);
   return 0;
}

int set_nonblock(int fd) {
// Функция переводит сокет fd в неблокирующий режим работы,
//если в сокете нет данных процесс не заблокируется на чтении или записи.
   int flags;
   // Если в операционной системе определен макрос O_NONBLOCK.
   #if defined(O_NONBLOCK)
      if( (flags = fcntl(fd, F_GETFL, 0) == -1) ) {
         flags = 0;
      }
      return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
   #else
      flags = 1;
      return ioctl(fd, FIONBIO, &flags);
   #endif
}

int safe_pop_front() {
// Выбираем из начала очереди самый первый slave-сокет, удалёем его и возвращаем его значение.
   int fd;
   // Блокируем одновременный доступ к списку семафором.
   sem_wait(locker);
   if (gt_ready_read_sockets.empty()){
     fd = -1;
   }
   // Если список не пуст.
   else {
     fd = *gt_ready_read_sockets.cbegin();
     gt_ready_read_sockets.pop_front();
   }
   // Разблокируем семафор.
   sem_post(locker);
   return fd;
}
void safe_push_back(int fd) {
// Добавляем дескриптор входящего соединения в очередь.
   // Доступ через семафор.
   sem_wait(locker);
   gt_ready_read_sockets.push_back(fd);
   sem_post(locker);
}

void extract_path_from_http_get_request(std::string& path, const char* buf, ssize_t len) {
// Парсим GET-запрос и получаем относительный путь к файлу.
   std::string request(buf, len);
   std::string s1(" ");
   std::string s2("?");
   // "GET "
   std::size_t pos1 = 4;
   std::size_t pos2 = request.find(s2, 4);
   if (pos2 == std::string::npos) {
     pos2 = request.find(s1, 4);
   }
   path = request.substr(4, pos2 - 4);
}

ssize_t sock_fd_write(int sock, void *buf, ssize_t buflen, int fd) {
// Передача дескриптора файла fd, передача выполняется по файловому дескриптору sock (парный сокет).
// Для передачи необходимо заполнить буфер buf техническими данными, минимум 1 байт.
   ssize_t           size;
   struct msghdr     msg;
   struct iovec      iov;
   union {
      struct cmsghdr    cmsghdr;
      char              control[CMSG_SPACE(sizeof (int))];
   } cmsgu;
   struct cmsghdr  *cmsg;

   iov.iov_base = buf;
   iov.iov_len = buflen;

   msg.msg_name = NULL;
   msg.msg_namelen = 0;
   msg.msg_iov = &iov;
   msg.msg_iovlen = 1;

   if (fd != -1) {
      msg.msg_control = cmsgu.control;
      msg.msg_controllen = sizeof(cmsgu.control);

      cmsg = CMSG_FIRSTHDR(&msg);
      cmsg->cmsg_len = CMSG_LEN(sizeof (int));
      cmsg->cmsg_level = SOL_SOCKET;
      cmsg->cmsg_type = SCM_RIGHTS;

      *((int *) CMSG_DATA(cmsg)) = fd;
   } else {
      msg.msg_control = NULL;
      msg.msg_controllen = 0;
   }
   // Передача информации.
   size = sendmsg(sock, &msg, 0);
   if (size < 0)
      perror ("sendmsg");
   return size;
}

ssize_t sock_fd_read(int sock, void *buf, ssize_t bufsize, int *fd) {
// Получение дескриптора файла fd, приём выполняется по файловому дескриптору sock (парный-сокет).
// Для передачи необходимо заполнить буфер buf техническими данными, минимум 1 байт.
   
   ssize_t     size;

   if (fd) {
      struct msghdr   msg;
      struct iovec    iov;
      union {
         struct cmsghdr  cmsghdr;
         char            control[CMSG_SPACE(sizeof (int))];
      } cmsgu;
      struct cmsghdr  *cmsg;

      iov.iov_base = buf;
      iov.iov_len = bufsize;

      msg.msg_name = NULL;
      msg.msg_namelen = 0;
      msg.msg_iov = &iov;
      msg.msg_iovlen = 1;
      msg.msg_control = cmsgu.control;
      msg.msg_controllen = sizeof(cmsgu.control);
      size = recvmsg (sock, &msg, 0);
      if (size < 0) {
         perror ("recvmsg");
         exit(1);
      }
      cmsg = CMSG_FIRSTHDR(&msg);
      if (cmsg && cmsg->cmsg_len == CMSG_LEN(sizeof(int))) {
         if (cmsg->cmsg_level != SOL_SOCKET) {
             fprintf (stderr, "invalid cmsg_level %d\n",
                  cmsg->cmsg_level);
             exit(1);
         }
         if (cmsg->cmsg_type != SCM_RIGHTS) {
             fprintf (stderr, "invalid cmsg_type %d\n",
                  cmsg->cmsg_type);
             exit(1);
         }

         *fd = *((int *) CMSG_DATA(cmsg));
      } else
         *fd = -1;
   } else {
      // Чтение данных.
      size = read (sock, buf, bufsize);
      if (size < 0) {
         perror("read");
         exit(1);
      }
   }
   return size;
}

int main(int argc, char **argv) {
   // Пишем в журна, прочитать можно grep final /var/log/syslog.
   syslog(LOG_NOTICE, "Старт Web-сервера (stepic).");
   // Парсим опции, которые переданы в командной строке.
   int opt;
   while ((opt = getopt(argc, argv, "h:p:d:")) != -1) {
      if(opt == 'h') {
         host = optarg;
      } else if (opt == 'p') {
         port = optarg;
      } else if (opt == 'd') {
         dir = optarg;
      } else {
         printf("Необходимо указать опции: %s -h <IP-адрес> -p <порт> -d <папка файлов сервера>\n", argv[0]);
         exit(1);
      }
   }
   if (host == 0 || port == 0 || dir == 0) {
      printf("Необходимо указать опции: %s -h <IP-адрес> -p <порт> -d <папка файлов сервера>\n", argv[0]);
      exit(1);
   }
#ifndef _R_DEBUG
   // Превращаем процесс в демон.
   if(start_daemon() != 0) {
      exit(EXIT_FAILURE);
   }
#endif
   // Создаём семафор в памяти в куче.
   locker = new sem_t;
   // Семафор доступен всем процессам, начальное значение 1 (открыт).
   sem_init(locker, 1, 1);
   // Создаём объект - Основной Цикл опроса ассинхронных событий с возможностью учитывать события дочерних процессов.
   struct ev_loop *loop = ev_default_loop(EVFLAG_FORKCHECK);
// Создание Воркеров (параллельные процессы - исполнители запросов пользователей).
   for(int i = 0; i < _COUNT_WORKERS; i++){
      PDEBUG("Запуск воркера(%d)\n", i);
      // Если воркер вернул 0, значит это код дочернего процесса.
      if(create_worker() == 0) {
         syslog(LOG_NOTICE, "ERROR: Воркер № %d прекратил свою работу.", i);
         // Воркер должен работать вечно, но если он возвратил управление сюда,
         //то завершаем этот дочерний процесс - процесс воркера.
         return 0;
      }
   }
// Создаём Мастер сокет и делаем его неблокирующимся.
   int master_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
   if (master_socket == -1) {
      PDEBUG("ERROR: Ошибка при создании мастер сокета.\n");
      syslog(LOG_NOTICE, "ERROR: Ошибка при создании мастер сокета.");
      exit(1);
   }
   set_nonblock(master_socket);
/*
   struct sockaddr_in addr;   
   // IPv4 адреса.
   addr.sin_family   = AF_INET;
   // Порт и IP адрес.  
   // IP адрес для сокета сервера.
   const char *l_host;
   // Длина структуры адреса сокета.
   socklen_t addrlen;
   struct sockaddr_in
      // Структура адреса для сокета сервера.
      l_address;
   // Обнуляем структуру адреса.
   memset(&l_address, 0, sizeof(l_address));
   // Присваиваем адрес.
   l_address.sin_addr.s_addr = inet_addr(host);
   l_address.sin_port        = htons(atoi(port));
   // Обнуляем незаполненную часть структуы.
   bzero(&l_address.sin_zero, sizeof(l_address.sin_zero));   
   //if (inet_pton(AF_INET, host, &(addr.sin_addr.s_addr)) != 1) {
   //   syslog(LOG_NOTICE, "ERROR: Ошибка при получении IP адреса.");
   //   exit(2);
   //}
   if (bind(master_socket, (struct sockaddr* )&l_address, sizeof(l_address)) < 0){
      PDEBUG("ERROR: Ошибка при присоединении IP адреса.\n");
      syslog(LOG_NOTICE, "ERROR: Ошибка при присоединении IP адреса.");
      exit(3);
   }
*/
   struct sockaddr_in addr;
   addr.sin_family = AF_INET;
   addr.sin_port = htons(atoi(port));
   if (inet_pton(AF_INET, host, &(addr.sin_addr.s_addr)) != 1) {
      PDEBUG("ERROR: Ошибка получения IP адреса.\n");
      syslog(LOG_NOTICE, "ERROR: Ошибка получения IP адреса.");
      exit(2);
   }
   if (bind(master_socket, (struct sockaddr* )&addr, sizeof(addr)) < 0) {
      PDEBUG("ERROR: Ошибка при присоединении IP адреса.\n");        
      syslog(LOG_NOTICE, "ERROR: Ошибка при присоединении IP адреса.");
      exit(3);
   }
   // Сокет на прослушку входящих запросов.
   listen(master_socket, SOMAXCONN);
   PDEBUG("Мастер сокет %d создан\n", master_socket);
   // Создаём вотчер, watcher (событие) для дескриптора файла мастер сокета
   // будем отслеживать событие доступно Чтение на мастер сокете (поступило входящее сообщение)
   // событие будем обрабатывать функцией master_accept_connection().
   struct ev_io master_watcher;
   ev_init    (&master_watcher, master_accept_connection);
   ev_io_set  (&master_watcher, master_socket, EV_READ);
   ev_io_start(loop, &master_watcher);
   // Запускаем вечный цикл опроса событий.
   ev_loop(loop, 0);
   // Сюда попадём если, что-то в цикле пошло не так.
   close(master_socket);
   return 0;
}

void master_accept_connection(struct ev_loop *loop, struct ev_io *w, int revents) {
// CALLBACK Функция, которая обрабатывает событие - На мастер сокете поступило входящее соединение.
   // Получаем дескриптор подключившегося сокета (слэйв сокет).
   int slave_socket = accept(w->fd, 0, 0);
   if (slave_socket == -1) {
      PDEBUG("ERROR: Ошибка при получении сокета входящего соединения %d выход\n", slave_socket);
      syslog(LOG_NOTICE, "ERROR: Ошибка при получении сокета входящего соединения.");
      exit(3);
   }
   set_nonblock(slave_socket);
   // Создаём вотчер для отслеживания события Доступно чтение 
   // на слэйв сокете, функция обработчик события slave_send_to_worker().
   struct ev_io* slave_watcher = new ev_io;
   ev_init    (slave_watcher, slave_send_to_worker);
   ev_io_set  (slave_watcher, slave_socket, EV_READ);
   ev_io_start(loop, slave_watcher);
   PDEBUG("Сработал обработчик события master_accept_connection(), получен слэйв сокет %d\n", slave_socket);
}

void slave_send_to_worker(struct ev_loop *loop, struct ev_io *w, int revents){
// CALLBACK Функция, которая обрабатывает событие - На slave-сокете доступны данные (переданных пользователем запрос).

   int slave_socket = w->fd;
   // Останавливаем вотчер (удалёем его из цикла опроса событий, однократное событие).
   ev_io_stop(loop, w);
   // Проходим воркерам.
   for(auto it = gt_workers.begin(); it != gt_workers.end(); it++) {
      // Если воркер в данный момент не занят.
      if ((*it).second) {
         // Занимаем воркер.
         (*it).second = false;
         // Передаём в воркет дескриптор slave-сокета.
         char tmp[1];
         sock_fd_write((*it).first, tmp, sizeof(tmp), slave_socket);
         return;
      }
   }
   // Добавляем slave-сокет в список (очередь) входящих соединений.
   safe_push_back(slave_socket);
}

pid_t create_worker(void) {
// Создание воркеров - параллельных процессов для обработки запросов пользователей. 
   int sp[2];
   // В каждом процессе воркера и родительском процессе создам пару-сокетов (pair-сокет)
   // по которой будет передаваться дескриптор пользовательского сокета входящего соединения. 
   if (socketpair(AF_LOCAL, SOCK_STREAM, 0, sp) == -1) {
      syslog(LOG_NOTICE, "ERROR: Ошибка при создании pair-сокета.");
      exit(1);
   }
   // Создаём новый цикл для отслеживания событий.
   struct ev_loop* loop = EV_DEFAULT;
   // Создаём дочерний процесс, который наследует все файловые дескрипторы родителя.
   pid_t pid = fork();
// Если родитель.
   if (pid) {
      // Закрываем один конец пары-сокетов, родитель будет общаться с воркерами по sp[0].
      close(sp[1]);
      // Добавляем sp[0] дескриптор пары-сокетов, который соединит мастер с текущим воркером в map-контейнер.
      gt_workers.insert(std::pair<int, bool>(sp[0], true));
      // Создаём вотчер, который будет отслеживать событие Доступно чтение с дескриптора sp[0] пары-сокета,
      // воркер посылает информацию мастеру по паре-сокету, когда завершит все текущие задачи, функция обработчик set_worker_free().
      struct ev_io* half_watcher = new ev_io;
      ev_init(half_watcher, set_worker_free);
      ev_io_set(half_watcher, sp[0], EV_READ);
      ev_io_start(loop, half_watcher);
      PDEBUG("Родитель соединён с Воркером %d по парному-сокету %d\n", pid, sp[0]);
   }
// Если дочерний процесс - воркер.
   else {
      // Закрываем один конец парного-сокета, потомок будет общаться с воркерами по sp[1].
      close(sp[0]);
      // Если не использовать флаг EVFLAG_FORKCHECK, то, чтобы добавить вотчер в дочерний процесс
      // нужно использовать функцию ev_default_fork().
      // Создаём вотчер - для отслеживания возможности чтения с парного-сокета sp[1],
      // то есть родитель передал в вотчер дескриптор сокета входящего соединения, обработчик do_work().
      struct ev_io worker_watcher;
      ev_init(&worker_watcher, do_work);
      ev_io_set(&worker_watcher, sp[1], EV_READ);
      ev_io_start(loop, &worker_watcher);
      PDEBUG("Воркер %d соединён с Родителем по парному-сокету %d\n", getpid(), sp[1]);
      // Запускаем вечный цикл опроса событий в дочернем процессе - в воркере.
      ev_loop(loop, 0);
   }
   // Тут мы окажемся в родителе.
   return pid;
}

void do_work(struct ev_loop *loop, struct ev_io *w, int revents) {
// CALLBACK Функция, которая обрабатывает событие Доступно чтение на парном-сокете воркера,
// то есть Родитель передал дескриптор сокета входящего соединения.
   PDEBUG("Сработал обработчик do_work(), в воркер пришли данные по парному-сокету %d (передан дескриптор входящего сокета)\n", w->fd);
   // По парному сокету читаем дескриптор входящего соединения (slave_socket).
   int slave_socket;
   char tmp[1];
   sock_fd_read(w->fd, tmp, sizeof(tmp), &slave_socket);
   PDEBUG("В воркере прочитан сокет %d.\n", slave_socket);
   if (slave_socket == -1) {
      PDEBUG("ERROR: Невозможно получить slave-сокет из pair-сокета %d выход.\n", w->fd);
      syslog(LOG_NOTICE, "ERROR: Невозможно получить slave-сокет из pair-сокета %d.", w->fd);
      exit(4);
   }
   // Текущий воркер выполняет полезную работу.
   process_slave_socket(slave_socket);
   // Передача родителю дескриптора, он поймёт, что воркер свободен.
   sock_fd_write(w->fd, tmp, sizeof(tmp), slave_socket);
   PDEBUG("Воркер %d передал данные пользователю\n", w->fd);  
}

void set_worker_free(struct ev_loop *loop, struct ev_io *w, int revents) {
// CALLBACK Функция, которая обрабатывает событие Доступно чтение на парном-сокете у родителя.
// Если родителю передал информацию воркер, то он свободен.
   int fd = w->fd;
   char tmp[1];
   int slave_socket;
   // Читаем дескриптор из парного-сокета (чтобы просто считать данные).
   sock_fd_read(fd, tmp, sizeof(tmp), &slave_socket);
   // Проходим по списку (очереди) входящих запросов и обрабатываем их все, пока очередь не будет пустой.
   while ((slave_socket = safe_pop_front()) != -1) {
      // Родительский процесс выполняет полезную работу.
      process_slave_socket(slave_socket);
   }
   // Метка, что текущий воркер свободен.
   gt_workers[fd] = true;
}

void process_slave_socket(int slave_socket) {
// Чтение запроса пользователя и отправка ответа slave-сокету.
   char buf[1024];
   // Читаем запрос.
   ssize_t recv_ret = recv(slave_socket, buf, sizeof(buf), MSG_NOSIGNAL);
   if (recv_ret == -1) {
      syslog(LOG_NOTICE, "ERROR: Возникла ошибка при получении запроса от пользователя.");
      return;
   }
   PDEBUG("Прочитан запрос\n%s\n", buf);
   // Получаем полный путь к файлу.
   std::string path;
   extract_path_from_http_get_request(path, buf, recv_ret);
   std::string full_path = std::string(dir) + path;
   PDEBUG("Полный путь файла %s\n", full_path.c_str());
   // Если файл существует, то определяем его размер и передаём.
   char reply[1024];
   //if (access(full_path.c_str(), F_OK) != -1){    
   struct stat f_buffer;
   if (full_path.back() != '/' && stat(full_path.c_str(), &f_buffer) == 0) {      
      int fd = open(full_path.c_str(), O_RDONLY);
      int sz = lseek(fd, 0, SEEK_END);
      PDEBUG("Размер файла %d\n", sz);      
      sprintf(reply, "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/html\r\n"
                    "Content-length: %d\r\n"
                    "Connection: close\r\n"
                    "\r\n", sz);
      // Отправляет HTTP-заголовок ответа.
      ssize_t send_ret = send(slave_socket, reply, strlen(reply), MSG_NOSIGNAL);
      off_t offset = 0;
      // Отправялем все байты файла.
      while (offset < sz) {         
         offset = sendfile(slave_socket, fd, &offset, sz - offset);
      }
      close(fd);
   // Если файла нет.
   } else {
      PDEBUG("Файла нет %s\n", full_path.c_str());      
      strcpy(reply, "HTTP/1.1 404 Not Found\r\n"
                   "Content-Type: text/html\r\n"
                   "Content-length: 107\r\n"
                   "Connection: close\r\n"
                   "\r\n");
      // Отправляет HTTP-заголовок ответа.      
      ssize_t send_ret = send(slave_socket, reply, strlen(reply), MSG_NOSIGNAL);
      // Тело HTML страницы об отсутствии файла.
      strcpy(reply, "<html>\n<head>\n<title>Not Found</title>\n</head>\r\n");
      send_ret = send(slave_socket, reply, strlen(reply), MSG_NOSIGNAL);
      strcpy(reply, "<body>\n<p>404 Request file not found.</p>\n</body>\n</html>\r\n");
      send_ret = send(slave_socket, reply, strlen(reply), MSG_NOSIGNAL);
   }
}

