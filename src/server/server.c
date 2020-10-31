#include "server.h"
#include "protocol.h"
#include <pthread.h>
#include <signal.h>
#include <time.h>

#define USAGE_MSG   "./bin/petr_server [-h] [-j N] PORT_NUMBER AUDIT_FILENAME\n"                                  \
                    "\n  -h               Displays the help menu, and returns EXIT_SUCCESS."                      \
                    "\n  -j               Number of job threads.  Default to 2."                                  \
                    "\n  AUDIT_FILENAME   File to output Audit Log messages to."                                  \
                    "\n  PORT_NUMBER      Port number to listen on.\n"                                            \

const char exit_str[] = "exit";
char* threadterminated = "Client thread has terminated.";
char* receivedmessage = "Received message from client.";
char* logoutaudit = "User has left the server.";
char* inserted = "Inserted job into buffer.";


int ulreadcnt, rlreadcnt = 0;  //reader counts for both the userlist and the roomlist 
pthread_mutex_t ulm, rlm, ulw, rlw;  //sempahors for readers/writers problem for both the userlist and the roomlist
pthread_mutex_t audit_lock;
pthread_mutex_t jbuffer_lock;
List_t* userlist;     
List_t* roomlist;  
List_t* joblist;

int total_num_msg = 0;
int listen_fd;
int n = 2;
FILE* auditlog;
int auditfd;

typedef struct user {
  char* username;
  int fd; // file descriptor
} user;

typedef struct room {
  char* roomname;
  char* creator;
  List_t* cuserlist;
} room;

typedef struct job {
  petr_header* pheadr;
  char* message;
  user* client;
} job;

void freeuser(user* u)
{
  free(u->username);
}

void freeroom(room* r)
{
  free(r->roomname);
  deleteList(r->cuserlist);
  // don't need to free creator since we use the char* from the user struct
}

void freejob(job* j)
{
  free(j->pheadr);
  if(j->message != NULL)
  {
    free(j->message);
  }
}

void sigint_handler(int sig) {
    printf("shutting down server\n");
    close(listen_fd);
  
  	node_t* ulhead = userlist->head;
  	while (ulhead != NULL)
    {
      user* u = ulhead->value;
      freeuser(u);
      ulhead = ulhead->next;
    }
  	deleteList(userlist);
  	node_t* rlhead = roomlist->head;
  	while (ulhead != NULL)
    {
      room* r = ulhead->value;
      freeroom(r);
      ulhead = ulhead->next;
    }
  	deleteList(roomlist);
  	node_t* jobhead = joblist->head;
  	while (jobhead != NULL)
    {
      job* j = jobhead->value;
      freejob(j);
      jobhead = jobhead->next;
    }
  	deleteList(joblist);
    close(auditfd);
  	pthread_mutex_destroy(&ulm);
    pthread_mutex_destroy(&ulw);
  	pthread_mutex_destroy(&rlm);
  	pthread_mutex_destroy(&rlw);
  	pthread_mutex_destroy(&jbuffer_lock);
  	pthread_mutex_destroy(&audit_lock);
		
    exit(0);
}

room* findRoom(char* roomname)  // requires roomlist reader's mutex
{
  node_t* head = roomlist->head;
    while (head != NULL) {
      room* r = head->value;
        if (r != NULL && strcmp(roomname, r->roomname) == 0) {
            return r;
        }
      	head = head->next;
    }
    return NULL;
}

user* findUser(char* username)  // requires userlist reader's mutex
{
  node_t* head = userlist->head;
    while (head != NULL) {
      user* u = head->value;
        if (u != NULL && strcmp(username, u->username) == 0) {
            return u;
        }
      	head = head->next;
    }
    return NULL;
}

int findRoomIndex(char* roomname)  // requires roomlist readers/writers mutex
{
  // returns -1 if room doesn't exist, else the index
  int index = 0;
    node_t* head = roomlist->head;
    while (head != NULL) {
        if (strcmp(roomname, ((room*)(head->value))->roomname) == 0) {
            return index;
        }
        index++;
      	head = head->next;
    }
    return -1;
}

void sendRMCLOSED(room* r)  // requires roomlist writers mutex
{
  node_t* head = r->cuserlist->head;
  while (head != NULL)
  {
    if (strcmp(r->creator, ((user*)(head->value))->username) != 0)
    {
      petr_header* p = malloc(sizeof(petr_header));
      p->msg_type = 0x22;
      p->msg_len = strlen(r->roomname) + 1;
      wr_msg(((user*)(head->value))->fd, p, r->roomname);
      free(p);
    }
    head = head->next;
  }
}

int userInRoom(room* r, user* u)
{
  node_t* head = r->cuserlist->head;
  while (head != NULL)
  {
    user* um = head->value;
    if (um == u)
    {
      return 0;
    }
    head = head->next;
  }
  return -1;
}

int findUserIndex(char* username) //requires userlist readers/writers mutex
{
  // returns -1 if user doesn't exist, else the index
  int index = 0;
    node_t* head = userlist->head;
    while (head != NULL) {
        if (strcmp(username, ((user*)(head->value))->username) == 0) {
            return index;
        }
        index++;
      	head = head->next;
    }
    return -1;
}

int findUserIndexRoom(room* r, user* u) //requires roomlist writers mutex
{
  // returns -1 if user doesn't exist, else the index
    //printf("I WORK\n");
    int index = 0;
    node_t* head = r->cuserlist->head;
    while (head != NULL) {
        user* um = head->value;
        if (um == u) {
            return index;
        }
        index++;
        head = head->next;
    }
    return -1;
}

void sendRoomMessage(room* r, user* sender, char* msg)
{
    //printf("newnewnew: %s\n", msg);
  petr_header* ph = malloc(sizeof(petr_header));
  ph->msg_type = 0x27;
  ph->msg_len = strlen(msg)+1;
  node_t* uhead = r->cuserlist->head;
  while (uhead != NULL)
  {
    user* u = uhead->value;
    if (u != sender)
    {
      wr_msg(u->fd, ph, msg);
    }
    uhead = uhead->next;
  }
  free(ph);
}

void logout(user* user_ptr)
{
  char* uname = user_ptr->username;
  pthread_mutex_lock(&rlw);
  node_t* head = roomlist->head;
  List_t* closedrooms = malloc(sizeof(List_t));
  closedrooms->head = NULL;
  closedrooms->length = 0;
  while (head != NULL)
  {
    room* r = head->value;
    if (strcmp(r->creator, uname) == 0)
    {
        sendRMCLOSED(r);
    	insertRear(closedrooms, r);
    }
    else
    {
        node_t* uhead = r->cuserlist->head;
        int userindex = 0;
        while (uhead != NULL)
        {
            user* u = uhead->value;
            if (strcmp(u->username, uname) == 0)
            {
                removeByIndex(r->cuserlist, userindex);
                break;
            }
            uhead = uhead->next;
            userindex++;
        }
    }
    head = head->next;
  }
  node_t* rhead = closedrooms->head;
  while (rhead != NULL)
  {
    room* rn = rhead->value;
    int b = findRoomIndex(rn->roomname);
    removeByIndex(roomlist, b);
    freeroom(rn);
    free(rn);
    rhead = rhead->next;
  }
  pthread_mutex_unlock(&rlw);
  pthread_mutex_lock(&ulw);
  int z = findUserIndex(uname);
  removeByIndex(userlist, z);
  pthread_mutex_unlock(&ulw);
  deleteList(closedrooms);
  free(closedrooms);
  pthread_mutex_lock(&audit_lock);
  fwrite(logoutaudit, 1, sizeof(logoutaudit), auditlog);
  pthread_mutex_unlock(&audit_lock); 
  petr_header* p = malloc(sizeof(petr_header));
  p->msg_type = 0x0;
  p->msg_len = 0;
  wr_msg(user_ptr->fd, p, NULL);
  close(user_ptr->fd);
  free(p);
  freeuser(user_ptr);
  free(user_ptr);
}

//Function running in thread
void *process_client(void *user_ptr) {
    pthread_detach(pthread_self());
    int client_fd = ((user*)(user_ptr))->fd;
  	
    int received_size;
    int retval;
    fd_set read_fds;
    
    while (1) {
        FD_ZERO(&read_fds);
        FD_SET(client_fd, &read_fds);
        retval = select(client_fd + 1, &read_fds, NULL, NULL, NULL);
        if (retval != 1 && !FD_ISSET(client_fd, &read_fds)) {
            printf("Error with select() function\n");
            break;
        }
        petr_header* j = malloc(sizeof(petr_header));
        j->msg_type = 0x0;
        j->msg_len = 0;
        int msge = rd_msgheader(client_fd, j);
        if (j->msg_type == 0x0)
        {
            logout(user_ptr);
          	free(j);
            j = NULL;
          	break;
        }
        else
        {
            char* msg;
            bool correctjob = 1;
            if (j->msg_len != 0)
            {
                msg = malloc(j->msg_len);
                received_size = read(client_fd, msg, j->msg_len);
                if (received_size <= 0) {
                    j->msg_type = 0xFF;
                    j->msg_len = 0;
                    wr_msg(client_fd, j, NULL);
                    free(msg);
                    correctjob = 0;
                } 
            }
            else
            {
                msg = NULL;
            }
            pthread_mutex_lock(&audit_lock);
            fwrite(receivedmessage, 1, sizeof(receivedmessage), auditlog);
            pthread_mutex_unlock(&audit_lock);
            if (correctjob)
            {
              job* newjob = malloc(sizeof(job));
              newjob->client = user_ptr;
              newjob->message = msg;
              newjob->pheadr = j;
              pthread_mutex_lock(&jbuffer_lock);
              insertRear(joblist, newjob);
              pthread_mutex_unlock(&jbuffer_lock);
              pthread_mutex_lock(&audit_lock);
              fwrite(inserted, 1, sizeof(inserted), auditlog);
              pthread_mutex_unlock(&audit_lock);
              if (j->msg_type == 0x11)
              {
                  break;  //logout, break the loop since the client is leaving, the job thread will close the connection after the logout is complete
              }
            }
        }
    }
    pthread_mutex_lock(&audit_lock);
    fwrite(threadterminated, 1, sizeof(threadterminated), auditlog);
    pthread_mutex_unlock(&audit_lock);
    return NULL;
}

void *jobthread() 
{
  	while(1)
    {
      job* j = NULL;
      pthread_mutex_lock(&jbuffer_lock);
      if (joblist->length > 0)
      {
      	j = removeFront(joblist);  
      }
      pthread_mutex_unlock(&jbuffer_lock);
      if (j != NULL)
      {
        petr_header* p = malloc(sizeof(petr_header));
        if (j->pheadr->msg_type == 0x11) {  //logout
            logout(j->client);
          
        } else if (j->pheadr->msg_type == 0x20) {  //rmcreate
          char* rn = malloc(j->pheadr->msg_len);
          rn = strcpy(rn, j->message);
          // check if roomname already exists
          pthread_mutex_lock(&rlm);
          rlreadcnt++;
          if (rlreadcnt == 1)
          {
            pthread_mutex_lock(&rlw);
          }
          pthread_mutex_unlock(&rlm);
          int i = findRoomIndex(rn);
          pthread_mutex_lock(&rlm);
          rlreadcnt--;
          if (rlreadcnt == 0)
          {
            pthread_mutex_unlock(&rlw);
          }
          pthread_mutex_unlock(&rlm);
          if (i == -1)  // room does not exist
          {
            room* r = malloc(sizeof(room));
            r->creator = j->client->username;
            r->roomname = rn;
            r->cuserlist = malloc(sizeof(List_t));
            r->cuserlist->head = NULL;
            r->cuserlist->length = 0;
            insertRear(r->cuserlist, j->client);
            pthread_mutex_lock(&rlw);
            insertRear(roomlist, r);
            pthread_mutex_unlock(&rlw);
            p->msg_type = 0x0;
            p->msg_len = 0;
            wr_msg(j->client->fd, p, NULL);
          }
          else
          {
            p->msg_type = 0x2A;
            p->msg_len = 0;
            wr_msg(j->client->fd, p, NULL);
            free(rn);
            rn = NULL;
          }
        } else if (j->pheadr->msg_type == 0x21) { // rmdelete
          //call findRoomIndex using the roomname
          //call room* r = removeByIndex with the index
          //call sendRMCLOSED to everyone in the room except the creator
          char* rn = j->message;
          pthread_mutex_lock(&rlw);
          int i = findRoomIndex(rn);
          if (i != -1)
          {
            room* r = removeByIndex(roomlist, i);
            if (strcmp(r->creator, j->client->username) == 0)
            {
              sendRMCLOSED(r);  //sends RMCLOSED to all users in the room except creator
              pthread_mutex_unlock(&rlw);
              // send OK back to client
              p->msg_type = 0x00;
              p->msg_len = 0;
              wr_msg(j->client->fd, p, NULL);
              freeroom(r);
              free(r);
            }
            else
            {
              // send ERMDENIED since the client is not the creator
              insertRear(roomlist, r);
              pthread_mutex_unlock(&rlw);
              p->msg_type = 0x2D;
              p->msg_len = 0;
              wr_msg(j->client->fd, p, NULL);
            }
          }
          else
          {
            pthread_mutex_unlock(&rlw);
            // send ERMNOTFOUND to client
            p->msg_type = 0x2C;
            p->msg_len = 0;
            wr_msg(j->client->fd, p, NULL);  
          }
          
        } else if (j->pheadr->msg_type == 0x23) { // rmlist
            pthread_mutex_lock(&rlm);
            rlreadcnt++;
            if (rlreadcnt == 1)
            {
                pthread_mutex_lock(&rlw);
            }
            pthread_mutex_unlock(&rlm);
            // critical section of reading, create a string that looks like "<roomname>:<username>,...,<username>\n<roomname>:<username>,...,<username>\n"
            node_t* head = roomlist->head;
            char roomString[] = "";
            while (head) {
                room* r = head->value;
                char* roomname = r->roomname;
                strcat(roomString, roomname);
                strcat(roomString, ": ");
                node_t* userHead = r->cuserlist->head;
                while (userHead) {
                    if (userHead->next != NULL) {
                        strcat(roomString, ((user*)(userHead->value))->username);
                        strcat(roomString, ",");
                    } else {
                        strcat(roomString, ((user*)(userHead->value))->username);
                    }
                    userHead = userHead->next;
                }
                strcat(roomString, "\n");
                head = head->next;
            }
            pthread_mutex_lock(&rlm);
            rlreadcnt--;
            if (rlreadcnt == 0)
            {
                pthread_mutex_unlock(&rlw);
            }
            pthread_mutex_unlock(&rlm);
            p->msg_type = 0x23;
          	if (strlen(roomString) > 0) {
          		p->msg_len = strlen(roomString)+1;
            } else {
             	p->msg_len = strlen(roomString); 
            }
          	wr_msg(j->client->fd, p, roomString);
          
        } else if (j->pheadr->msg_type == 0x24) { // rmjoin
          pthread_mutex_lock(&rlw);
          room* r = findRoom(j->message);
          if (r != NULL)
          {
            if (userInRoom(r, j->client) == -1)
            {
              insertRear(r->cuserlist, j->client);
            }
            pthread_mutex_unlock(&rlw);
            p->msg_type = 0x0;
            p->msg_len = 0;
            wr_msg(j->client->fd, p, NULL);
          }
          else
          {
            pthread_mutex_unlock(&rlw);
            p->msg_type = 0x2C;
            p->msg_len = 0;
            wr_msg(j->client->fd, p, NULL);
          }
        } else if (j->pheadr->msg_type == 0x25) { // rmleave
          pthread_mutex_lock(&rlw);
          room* r = findRoom(j->message);
          if (r != NULL)
          {
            if (strcmp(r->creator, j->client->username) == 0)
            {
              pthread_mutex_unlock(&rlw);
              p->msg_type = 0x2D;
              p->msg_len = 0;
              wr_msg(j->client->fd, p, NULL);
            }
            else
            {
              if (userInRoom(r, j->client) == 0)
              {
                int i = findUserIndexRoom(r, j->client);
                removeByIndex(r->cuserlist, i);
              }
              pthread_mutex_unlock(&rlw);
              p->msg_type = 0x0;
              p->msg_len = 0;
              wr_msg(j->client->fd, p, NULL);
            }
          }
          else
          {
            pthread_mutex_unlock(&rlw);
            p->msg_type = 0x2C;
            p->msg_len = 0;
            wr_msg(j->client->fd, p, NULL);
          }
        } else if (j->pheadr->msg_type == 0x26) { // rmsend
          //get readers mutex for roomlist
          char *roomname;;
          char *smg;
          char *saveptr;
          roomname = strtok_r(j->message, "\r", &saveptr);
          smg = strtok_r(NULL, "", &saveptr);
          smg = smg + 1;
          pthread_mutex_lock(&rlm);
          rlreadcnt++;
          if (rlreadcnt == 1)
          {
            pthread_mutex_lock(&rlw);
          }
          pthread_mutex_unlock(&rlm);
          room* r = findRoom(roomname);
          if (r != NULL) // the room exists
          {
            if (findUserIndexRoom(r, j->client) != -1)
            {
              char newmessage[] = "";
              strcat(newmessage, roomname);
              strcat(newmessage, "\r\n");
              strcat(newmessage, j->client->username);
              strcat(newmessage, "\r\n");
              strcat(newmessage, smg);
              sendRoomMessage(r, j->client, newmessage);
              p->msg_type = 0x0;
              p->msg_len = 0;
              wr_msg(j->client->fd, p, NULL);
            }
            else // user not in room
            {
              p->msg_type = 0x2D;
              p->msg_len = 0;
              wr_msg(j->client->fd, p, NULL);
            }
          }
          else
          {
            p->msg_type = 0x2C;
            p->msg_len = 0;
            wr_msg(j->client->fd, p, NULL);
          }
          pthread_mutex_lock(&rlm);
          rlreadcnt--;
          if (rlreadcnt == 0)
          {
            pthread_mutex_unlock(&rlw);
          }
          pthread_mutex_unlock(&rlm);
        } else if (j->pheadr->msg_type == 0x30) { // usersend
          char* username;
          char* smg;
          char* saveptr;
          username = strtok_r(j->message, "\r", &saveptr);
          smg = strtok_r(NULL, "", &saveptr);
          smg = smg + 1;
          pthread_mutex_lock(&ulm);
          ulreadcnt++;
          if (ulreadcnt == 1)
          {
            pthread_mutex_lock(&ulw);
          }
          pthread_mutex_unlock(&ulm);
          user* u = findUser(username);
          if (username != j->client->username && u != NULL)
          {
            char newmessage[] = "";
            strcat(newmessage, j->client->username);
            strcat(newmessage, "\r\n");
            strcat(newmessage, smg);
            petr_header ph;
            ph.msg_type = 0x31;
            ph.msg_len = strlen(newmessage) + 1;
            wr_msg(u->fd, &ph, newmessage);
            p->msg_type = 0x0;
            p->msg_len = 0;
            wr_msg(j->client->fd, p, NULL);
          }
          else
          {
            p->msg_type = 0x3A;
            p->msg_len = 0;
            wr_msg(j->client->fd, p, NULL);
          }
          pthread_mutex_lock(&ulm);
          ulreadcnt--;
          if (ulreadcnt == 0)
          {
            pthread_mutex_unlock(&ulw);
          }
          pthread_mutex_unlock(&ulm);
        } else if (j->pheadr->msg_type == 0x32) { // userlist
            pthread_mutex_lock(&rlm);
            rlreadcnt++;
            if (rlreadcnt == 1)
            {
            pthread_mutex_lock(&rlw);
            }
            pthread_mutex_unlock(&rlm);
            // critical section of reading, create a string that looks like "<username>\n<username>\n..."
            node_t* head = userlist->head;
            char userString[] = "";
            char* name = j->client->username; // name of user from client
            while (head) {
                char* userName = ((user*)(head->value))->username; // username from l
                if (strcmp(userName, name) != 0) {
                    strcat(userString, userName);
                    strcat(userString, "\n");
                }
                head = head->next;
            }
            pthread_mutex_lock(&rlm);
            rlreadcnt--;
            if (rlreadcnt == 0)
            {
            pthread_mutex_unlock(&rlw);
            }
            pthread_mutex_unlock(&rlm);
            p->msg_type = 0x32;
            if (strlen(userString) > 0) {
                p->msg_len = strlen(userString)+1;
            } else {
                p->msg_len = strlen(userString);
            }
            wr_msg(j->client->fd, p, userString);
        }
        freejob(j);
        free(j);
        free(p);
      }
    }
}

int server_init(int server_port) {
    int sockfd;
    struct sockaddr_in servaddr;

    // socket create and verification
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        printf("socket creation failed...\n");
        exit(EXIT_FAILURE);
    } else
        printf("Socket successfully created\n");

    bzero(&servaddr, sizeof(servaddr));
		
  	userlist = malloc(sizeof(List_t));   // free this userlist later
    userlist->head = NULL;
    userlist->length = 0;
  	roomlist = malloc(sizeof(List_t));   // free this roomlist later
    roomlist->head = NULL;
    roomlist->length = 0;
  	joblist = malloc(sizeof(List_t));  // free this joblist later
    joblist->head = NULL;
    joblist->length = 0;
  	pthread_mutex_init(&ulm, NULL);
  	pthread_mutex_init(&rlm, NULL);
  	pthread_mutex_init(&ulw, NULL);
  	pthread_mutex_init(&rlw, NULL);
  	pthread_mutex_init(&audit_lock, NULL);
    pthread_mutex_init(&jbuffer_lock, NULL);
    
    pthread_t tid;
    int i;
    for(i = 0; i < n; i++)
    {
        pthread_create(&tid, NULL, jobthread, NULL);   // create job threads
    }
    // assign IP, PORT
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(server_port);

    int opt = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, (char *)&opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    // Binding newly created socket to given IP and verification
    if ((bind(sockfd, (SA *)&servaddr, sizeof(servaddr))) != 0) {
        printf("socket bind failed\n");
        free(userlist);
        free(joblist);
        free(roomlist);
        exit(EXIT_FAILURE);
    } else
        printf("Socket successfully binded\n");

    // Now server is ready to listen and verification
    if ((listen(sockfd, 1)) != 0) {
        printf("Listen failed\n");
        free(userlist);
        free(joblist);
        free(roomlist);
        exit(EXIT_FAILURE);
    } else
        printf("Server listening on port: %d.. Waiting for connection\n", server_port);

    return sockfd;
}

void run_server(int server_port) {
    listen_fd = server_init(server_port); // Initiate server and start listening on specified port
    int client_fd;
    struct sockaddr_in client_addr;
    int client_addr_len = sizeof(client_addr);

    pthread_t tid;

    while (1) 
    {
        // Wait and Accept the connection from client
        printf("Wait for new client connection\n");
        int *client_fd = malloc(sizeof(int));
        *client_fd = accept(listen_fd, (SA *)&client_addr, (socklen_t*)&client_addr_len);
        if (*client_fd < 0) {
            printf("server acccept failed\n");
            exit(EXIT_FAILURE);
        } 
        else 
        {
            printf("Client connetion accepted\n");
          	petr_header* login = malloc(sizeof(petr_header));
            login->msg_type = 0x0;
            login->msg_len = 0;
	          rd_msgheader(*client_fd, login);
          	if (login->msg_type == 0x10 && login->msg_len > 0)
            {
              char* loginmsg = malloc(login->msg_len);
              int readbytes = read(*client_fd, loginmsg, login->msg_len);
              if (readbytes < login->msg_len)
              {
                // reading the message failed
                login->msg_type = 0xFF;
                login->msg_len = 0;
                wr_msg(*client_fd, login, NULL);
                close(*client_fd);
                free(client_fd);
                free(login);
                free(loginmsg);
              }
              else
              {
                // look through the userlist and see if the username matches any already existing users
                pthread_mutex_lock(&ulw);
                node_t* head = userlist->head;
                bool exists = 0;
                while (head != NULL)
                {
                  if (strcmp(loginmsg, ((user*)(head->value))->username) == 0)
                  {
                    // send user already exists back to client & close connection
                    // break out this while loop
                    login->msg_type = 0x1A;
                    login->msg_len = 0;
                    wr_msg(*client_fd, login, NULL);
                    exists = 1;
                    close(*client_fd);
                    free(client_fd);
                    free(login);
                    free(loginmsg);
                    break;
                  }
                  else
                  {
                    head = head->next;
                  }
                }
                if (!exists)
                {
                    login->msg_type = 0x0;
                    login->msg_len = 0;
                    wr_msg(*client_fd, login, NULL);
                    user* newuser = malloc(sizeof(user));
                    newuser->fd = *client_fd;
                    newuser->username = loginmsg;
                  	insertRear(userlist, newuser);
                    char loginaudit[] = "User ";
                    strcat(loginaudit, loginmsg);
                  	strcat(loginaudit, " has joined the server. ");
                    time_t t = time(NULL);
                    strcat(loginaudit, ctime(&t));
                    pthread_mutex_lock(&audit_lock);
                  	fwrite(loginaudit, 1, sizeof(loginaudit), auditlog);
                    pthread_mutex_unlock(&audit_lock); 
                    pthread_create(&tid, NULL, process_client, (void *)newuser);
                    free(client_fd);
                    free(login);
                }
                pthread_mutex_unlock(&ulw);
              }
            }
          	else
            {
              // reject login & close connection
              login->msg_type = 0xFF;
              login->msg_len = 0;
              wr_msg(*client_fd, login, NULL);
              close(*client_fd);
              free(client_fd);
              free(login);
            }
        }
    }
    close(listen_fd);
    return;
}


int main(int argc, char *argv[]) {
    int opt;
    char* audit_log;

    unsigned int port = 0;
    while ((opt = getopt(argc, argv, "hj:")) != -1) {
        switch (opt) {
        case 'h':
            printf(USAGE_MSG);
            return EXIT_SUCCESS;
        case 'j':
            n = atoi(optarg);
            break;
        case '?':
            if (optopt == 'j'){
                fprintf(stderr, "Option -%c requires an argument.\n\n" USAGE_MSG, optopt);
            }
        default: /* '?' */
            fprintf(stderr, "Server Application Usage: %s -p <port_number>\n",
                    argv[0]);
            exit(EXIT_FAILURE);
        }
    }
    if (optind + 2 != argc) 
    {
        fprintf(stderr, "Exactly two positional arguments should be specified.\n\n" USAGE_MSG);
        return EXIT_FAILURE;
    }

    if (n <= 0)
    {
        printf(USAGE_MSG);
        return EXIT_FAILURE;
    }

    port = atoi(argv[optind]);
    audit_log = argv[optind+1];

    auditlog = fopen(audit_log, "w");  // initialize audit log
    if (auditlog == NULL)
    {
        printf(USAGE_MSG);
        return EXIT_FAILURE;
    }
  	auditfd = fileno(auditlog);

    if (port == 0) {
        fprintf(stderr, "ERROR: Port number for server to listen is not given\n");
        fprintf(stderr, "Server Application Usage: %s -p <port_number>\n",
                argv[0]);
        exit(EXIT_FAILURE);
    }

    run_server(port);

    return 0;
}
