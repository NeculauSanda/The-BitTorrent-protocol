#include <mpi.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define HASH_SIZE 32
#define MAX_CHUNKS 100

typedef struct {
    int client_id;
    bool is_seed;
    bool segment_exit[MAX_CHUNKS];
} Client;

typedef struct {
    char nume_fisier[MAX_FILENAME];
    int nr_bucati_fisier;
    char bucati[MAX_CHUNKS][HASH_SIZE + 1];
} Data_fisier;

typedef struct {
    Data_fisier file;
    Client client_info[10];
    int nr_clienti;
} Swarm;

typedef struct {
    int rank;
    int nr_fisiere_cerute;
    int nr_fisiere_detinute;
    Data_fisier *fisiere_cerute;
    Data_fisier *fisiere_detinute;
} Data_Info;


Data_Info read_file_input(int rank) {

    char nume_fisier[MAX_FILENAME];
    snprintf(nume_fisier, sizeof(nume_fisier), "in%d.txt", rank); // numele fisierului de intrare
    FILE *file = fopen(nume_fisier, "r");

    if (!file) {
        fprintf(stderr, "Eroare la deschiderea fisierului %s\n", nume_fisier);
        exit(EXIT_FAILURE);
    }

    int nr_fisiere;
    fscanf(file, "%d", &nr_fisiere);

    Data_fisier *fisiere_detinute = malloc(nr_fisiere * sizeof(Data_fisier));

    for (int i = 0; i < nr_fisiere; i++) {
        fscanf(file, "%s %d", fisiere_detinute[i].nume_fisier, &fisiere_detinute[i].nr_bucati_fisier);
        MPI_Send(fisiere_detinute[i].nume_fisier, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD); //NUME
        MPI_Send(&fisiere_detinute[i].nr_bucati_fisier, 1, MPI_INT, TRACKER_RANK, 1, MPI_COMM_WORLD); // NR BUCATI FISIER

        for (int j = 0; j < fisiere_detinute[i].nr_bucati_fisier; j++) {
            fscanf(file, "%s", fisiere_detinute[i].bucati[j]);
            MPI_Send(&fisiere_detinute[i].bucati[j], HASH_SIZE, MPI_CHAR, TRACKER_RANK, 2, MPI_COMM_WORLD); // HASH
        }
    }
    
    //semnalare sfarsit de partajare informatii pe care le are
    MPI_Send("WAIT", 5, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD);

    char raspuns[32];
    MPI_Recv(raspuns, 4, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD, NULL);
    if (strncmp(raspuns, "ACK", 3) == 0) {
        printf("Clinetul %d: A TERMINAT DE PARTAJAT DATELE SALE.\n", rank);
    }

    int nr_fisiere_cerute;
    fscanf(file, "%d", &nr_fisiere_cerute);
    
    Data_fisier *fisiere_cerute = calloc(nr_fisiere_cerute, sizeof(Data_fisier));

    for (int i = 0; i < nr_fisiere_cerute; i++) {
        fscanf(file, "%s", fisiere_cerute[i].nume_fisier);
    }

    fclose(file);
    
    int nr_total_fisiere = nr_fisiere + nr_fisiere_cerute + 1;
    // realocam spatiul de fisiere pe care o sa le aiba la sfarsit dupa ce o sa descarce si retul
    fisiere_detinute = realloc(fisiere_detinute, (nr_total_fisiere) * sizeof(Data_fisier));

    Data_Info general_info = {
        .rank = rank,
        .nr_fisiere_cerute = nr_fisiere_cerute,
        .fisiere_cerute = fisiere_cerute,
        .nr_fisiere_detinute = nr_fisiere,
        .fisiere_detinute = fisiere_detinute
    };
    return general_info;
}

int random_client(Swarm *swarm, int segment) {
    int nr_clienti_cu_segment = 0;
    int clienti_segment[10];

    // Cautam clienti care au segmentul
    int i = 0;
    while(i < swarm->nr_clienti) {
        if (swarm->client_info[i].segment_exit[segment]) {
            clienti_segment[nr_clienti_cu_segment++] = swarm->client_info[i].client_id;
        }
        i++;
    }
    //nu avem clienti cu segmentul ala
    if (nr_clienti_cu_segment == 0) {
        return -1;
    }
    // alegem un client random 
    return clienti_segment[rand() % nr_clienti_cu_segment];
}

void *download_thread_func(void *arg)
{
    MPI_Status status;
    Data_Info *general_info = (Data_Info *)arg;
    int finish = 0;

    Data_fisier *fisiere_cerute = general_info->fisiere_cerute;
    Data_fisier *fisiere_detinute = general_info->fisiere_detinute;

    for(int i = 0; i < general_info->nr_fisiere_cerute; i++) {
        Swarm info_primite;
        Client client_info;

        // trimitem cerere la tracker pt faptul ca clientul vrea fisier
        MPI_Send("REQUEST_FILE", 13, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD);
        MPI_Send(fisiere_cerute[i].nume_fisier, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD); //NUMELE

        // primim informatia din swarm de la tracker doar pentru fisierul cerut
        MPI_Recv(&info_primite, sizeof(Swarm), MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD, &status); 

        // ACTUALIZAM FISIERELE DETINUTE DE CLIENT 
        int aux_nr_fisiere = general_info->nr_fisiere_detinute;
        fisiere_detinute[aux_nr_fisiere].nr_bucati_fisier = info_primite.file.nr_bucati_fisier; // NR BUCATI FISIER 
        strcpy(fisiere_detinute[aux_nr_fisiere].nume_fisier, fisiere_cerute[i].nume_fisier); // NUME
        general_info->nr_fisiere_detinute++; // NR FISIERE DETINUTE ACTUALIZAT
        
        int segmente_primite = 0; // numarul de segmente pe care le are clientul
        int segmente_cer = info_primite.file.nr_bucati_fisier; // numarul de segmente pe care le are fisierul

        //initializare date fisier cerut de client
        memset(client_info.segment_exit, 0, segmente_cer * sizeof(client_info.segment_exit[0]));
        client_info.is_seed = 0;

        int again_update = 0;

        //PANA CAND AVEM TOATE SEGMENTELE
        while (segmente_primite < segmente_cer) {
            again_update = 0;
            char buff[256];
            char hash_primit[HASH_SIZE + 1];

            // alegem un client random care are segmentul cerut
            int rank_client_random = random_client(&info_primite, segmente_primite);
            if (rank_client_random < 0) {
                printf("Nu s-a gasit un client\n");
            }

            sprintf(buff, "REQUEST_UP ");
            strcat(buff, fisiere_cerute[i].nume_fisier);
            char segment_str[4];
            sprintf(segment_str, " %d", segmente_primite);
            strcat(buff, segment_str);
            
            // trimitem cerere la upload thread pentru segmentul cerut
            MPI_Send(buff, sizeof(buff), MPI_CHAR, rank_client_random, 1, MPI_COMM_WORLD);

            // primim hash-ul segmentului cerut
            MPI_Recv(hash_primit, HASH_SIZE, MPI_CHAR, rank_client_random, 0, MPI_COMM_WORLD, &status);

            //adaugam hash-ul
            client_info.segment_exit[segmente_primite] = 1; 
            strncpy(fisiere_detinute[aux_nr_fisiere].bucati[segmente_primite], hash_primit, HASH_SIZE);
            segmente_primite++;

            // trimitem ACK la upload thread ca totul a decurs cum trebuie
            MPI_Send("ACK", 4, MPI_CHAR, rank_client_random, 1, MPI_COMM_WORLD);

            if (segmente_primite != 0 && segmente_primite % 10 == 0) {
                // Odata la 10 segmente downloadate, trimitem un mesaj de update la tracker
                MPI_Send("UPDATE_TIME", 12, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD);
                
                // trimitem numele fisierului
                MPI_Send(fisiere_cerute[i].nume_fisier, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD);

                // trimitem informatii despre client
                MPI_Send(&client_info, sizeof(Client), MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD);            

                //PRIMIM SWAM ACTUALIZAT CU INFORMATIILE LA UN MOMEMT DAT
                MPI_Recv(&info_primite, sizeof(Swarm), MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD, &status);
                
                again_update = 1;
            }
        }

        //---------- ACTUALIZAM PENTRU ULTIMELE BUCATI DACA NU S-A FACUT ------------
        if (again_update == 0) {
            MPI_Send("UPDATE_TIME", 12, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD);

            // trimitem numele fisierului
            MPI_Send(fisiere_cerute[i].nume_fisier, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD);

            // trimitem informatii despre client
            MPI_Send(&client_info, sizeof(Client), MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD);            

            //PRIMIM SWAM ACTUALIZAT CU INFORMATIILE LA UN MOMEMT DAT
            MPI_Recv(&info_primite, sizeof(Swarm), MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD, &status);
        }

        // ------------- FISIERUL DE OUTPUT ------------
        int rank_client_out = general_info->rank;
        char rank_str[4];
        char nume_fisier_output[15];
        sprintf(rank_str, "%d", rank_client_out);
        strcpy(nume_fisier_output, "client");
        strcat(nume_fisier_output, rank_str);
        strcat(nume_fisier_output, "_");
        strcat(nume_fisier_output, fisiere_cerute[i].nume_fisier);

        FILE *output_file = fopen(nume_fisier_output, "w");
        if (!output_file) {
            fprintf(stderr, "Eroare la deschiderea fisierului %s\n", nume_fisier_output);
            exit(EXIT_FAILURE);
        }

        //----------- hash-urile ----------
        int aux_nr_bucati = fisiere_detinute[aux_nr_fisiere].nr_bucati_fisier;
        for (int j = 0; j < aux_nr_bucati; j++) {
            fprintf(output_file, "%s\n", fisiere_detinute[aux_nr_fisiere].bucati[j]);
        }

        fclose(output_file);

        // ------ TERMINAT DE DESCARCAT FISIERUL ---------
        MPI_Send("DONE_FILE", 10, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD);  
        MPI_Send(fisiere_detinute[aux_nr_fisiere].nume_fisier, MAX_FILENAME, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD); // trimitem numele fisierului TERMINAT DE DESCARCAT
        finish++;
    }

    if(finish == general_info->nr_fisiere_cerute) {
        //---------- TERMINAT DE DESCARCAT TOATE FISIERELE ------------
        MPI_Send("EXIT", 5, MPI_CHAR, TRACKER_RANK, 0, MPI_COMM_WORLD); 
    }

    return NULL;
}

float find_file_general(Data_Info *general_info, char* nume_fisier) {
    for (int i = 0; i < general_info->nr_fisiere_detinute; i++) {
        if (strcmp(general_info->fisiere_detinute[i].nume_fisier, nume_fisier) == 0)  {
            return i;
        }
    }
    return -1;
}

float find_file(Swarm *swarm, char* nume_fisier, int nr_fisiere) {
    for (int i = 0; i < nr_fisiere; ++i) {
        if (strcmp(swarm[i].file.nume_fisier, nume_fisier) == 0) {
            return i;
        }
    }
    return -1;
}

float find_client(Swarm *swarm, int rank, int file_index) {
    for (int i = 0; i < swarm[file_index].nr_clienti; ++i) {
        if (swarm[file_index].client_info[i].client_id == rank) {
            return i;
        }
    }
    return -1;
}

void *upload_thread_func(void *arg)
{
    Data_Info *general_info = (Data_Info *)arg;

    while (1) {
        MPI_Status status;
        char mesaj_primit[256] = {0};
        char nume_fisier[MAX_FILENAME];
        char cerere[13];
        int segmente_cerute;
        
        // primim cererea de la download thread
        MPI_Recv(mesaj_primit, sizeof(mesaj_primit), MPI_CHAR, MPI_ANY_SOURCE, 1, MPI_COMM_WORLD, &status);
        int sender_rank = status.MPI_SOURCE; // rank-ul clientului care are segmentul cerut

        char *token = strtok(mesaj_primit, " ");
        strcpy(cerere, token);

        if (strncmp(cerere, "REQUEST_UP", 10) == 0) {

            token = strtok(NULL, " ");
            strcpy(nume_fisier, token);

            token = strtok(NULL, " ");
            segmente_cerute = atoi(token);

            // cautam fisierul cerut
            int file_index;
            file_index = find_file_general(general_info, nume_fisier);

            if(file_index == -1) {
                printf("Fisierul nu exista\n");
                return NULL;
            }

            // trimitem hash-ul segmentului cerut cu rank-ul clientului care are segmentul
            MPI_Send(general_info->fisiere_detinute[file_index].bucati[segmente_cerute], HASH_SIZE, MPI_CHAR, sender_rank, 0, MPI_COMM_WORLD);

            char mesaj_ok[4];
            int ok = 0;
            MPI_Recv(mesaj_ok, sizeof(mesaj_ok), MPI_CHAR, sender_rank, 1, MPI_COMM_WORLD, &status);
            if (strncmp(mesaj_ok, "ACK", 3) == 0) {
                ok = 1;
            }

        } else if (strncmp(mesaj_primit, "CLOSE", 5) == 0) {   
            // primeste de la tracker cand a terminat de descarcat toate fisierele
            printf("INCHIDE ACTIVITATE THREAD-ULUI %d\n", general_info->rank);
            return NULL;
        }
    }
    return NULL;
}



void tracker(int numtasks, int rank) {
    
    MPI_Status status;
    Swarm swarm[MAX_FILES];
    int nr_fisiere_swarm = 0;
    int cnt_finished = numtasks - 1;

    while (1) {
        char nume_fisier[MAX_FILENAME];
        int nr_bucati_fisier;
        MPI_Probe(MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &status);
        // aflam sursa mesajului
        int sender_rank = status.MPI_SOURCE;
        
        // -------- NUME FISIER ---------
        MPI_Recv(nume_fisier, MAX_FILENAME, MPI_CHAR, sender_rank, 0, MPI_COMM_WORLD, &status);

        //SEMNALUL PT FAPTUL CA A TERMINAT DE PROCESAT INFORMATIILE CLIENTULUI
        if(strncmp(nume_fisier, "WAIT", 4) == 0) {
            cnt_finished--;
            // daca toate procesele au terminat de procesat informatia pe care o
            // are fiecare client atunci se poate incepe download-ul
            if (cnt_finished == 0) {
                int aux = 1;
                while (aux < numtasks) {
                    MPI_Send("ACK", 4, MPI_CHAR, aux, 0, MPI_COMM_WORLD);
                    aux++;
                }
                break;
            }
        } else {
            // ------------- PRIMIM NUMARUL DE BUCATI ------------
            MPI_Recv(&nr_bucati_fisier, 1, MPI_INT, sender_rank, 1, MPI_COMM_WORLD, &status);

            // cautam fisierul in swarm
            int file_index;
            file_index = find_file(swarm, nume_fisier, nr_fisiere_swarm);

            // PENTRU FIECARE FISIER SALVAM INFORMATIILE DESPRE CLIENTI SI BUCATI
            // fisierul nu exita in swarm
            if (file_index == -1) {
                swarm[nr_fisiere_swarm].client_info[0].is_seed = 1;
                swarm[nr_fisiere_swarm].nr_clienti = 1;
                swarm[nr_fisiere_swarm].file.nr_bucati_fisier = nr_bucati_fisier;
                strcpy(swarm[nr_fisiere_swarm].file.nume_fisier, nume_fisier);
                swarm[nr_fisiere_swarm].client_info[0].client_id = sender_rank;

                // --------------- PRIMIM HASH-URILE -------------
                for (int i = 0; i < nr_bucati_fisier; i++) {
                    char hash[HASH_SIZE + 1];
                    MPI_Recv(hash, HASH_SIZE, MPI_CHAR, sender_rank, 2, MPI_COMM_WORLD, &status);
                    // copiem hash-ul in swarm
                    strncpy(swarm[nr_fisiere_swarm].file.bucati[i], hash, HASH_SIZE);
                    swarm[nr_fisiere_swarm].client_info[0].segment_exit[i] = 1; // exista segmentul
                }
                nr_fisiere_swarm++; //actualizam numarul de fisiere din swarm(indexul actual)

            } else {
                // fisierul exista, cautam indexul clientului sau facem unu nou daca 
                // nu exista clientul pentru fisierul respectiv
                int client_index;
                client_index = find_client(swarm, sender_rank, file_index);
            
                //----------CLIENT NOU PENTRU CARE ARE FISIERUL ------------
                if (client_index == -1) {
                    for (int i = 0; i < nr_bucati_fisier; i++) {
                        // segmentul exista pentru client
                        swarm[file_index].client_info[swarm[file_index].nr_clienti].segment_exit[i] = 1;
                    }
                    swarm[file_index].client_info[swarm[file_index].nr_clienti].client_id = sender_rank;
                    swarm[file_index].client_info[swarm[file_index].nr_clienti].is_seed = 1; // client seed
                    swarm[file_index].nr_clienti++;
                }
            }
        }
    }
    
    // ------ PRELUCRAM CERERILE CLIENTILOR PRIMITE DE TRACKER ---------
    cnt_finished = numtasks - 1;
    char mesaj_client[MAX_FILENAME];

    while(1) {
        memset(mesaj_client, 0, sizeof(mesaj_client));
        MPI_Probe(MPI_ANY_SOURCE, 0, MPI_COMM_WORLD, &status);
        // aflam sursa mesajului
        int rank_client = status.MPI_SOURCE;
        // PRIMIM CEREREA DE LA CLIENT
        MPI_Recv(mesaj_client, MAX_FILENAME, MPI_CHAR, rank_client, 0, MPI_COMM_WORLD, &status);
        // int rank_client = status.MPI_SOURCE;

        // -------- MESAJ DE CERERE FISIERE DE LA CLIENT ---------
        if (strncmp(mesaj_client, "REQUEST_FILE", 12) == 0) {

            //nume fisier cerut
            char nume_fisier_dorit[MAX_FILENAME];
            MPI_Recv(nume_fisier_dorit, MAX_FILENAME, MPI_CHAR, rank_client, 0, MPI_COMM_WORLD, &status);

            // cautam fisierul
            int file_index;
            file_index = find_file(swarm, nume_fisier_dorit, nr_fisiere_swarm);
            
            if (file_index >= 0) {
                // TRIMITEM INFORMATIILE DESPRE FISIERUL CERUT
                MPI_Send(&swarm[file_index], sizeof(Swarm), MPI_CHAR, rank_client, 0, MPI_COMM_WORLD);
            } else if(file_index == -1) {
                printf("NU EXISTA FISIERUL %s.\n", nume_fisier_dorit);
            }

        } else if (strncmp(mesaj_client, "UPDATE_TIME", 11) == 0) {
            //-------- ACTUALIZAM LA O PERIOADA DE TIMP INFORMATIILE DESPRE FISIERE SI CLIENT ------------

            //primim numele fisierului actualizat de la thread-ul de download
            char nume_fisier_actualizat[MAX_FILENAME];
            MPI_Recv(nume_fisier_actualizat, MAX_FILENAME, MPI_CHAR, rank_client, 0, MPI_COMM_WORLD, &status);

            // cautam fisierul 
            int update_file_index;
            update_file_index = find_file(swarm, nume_fisier_actualizat, nr_fisiere_swarm);

            if (update_file_index > -1) {
                // primi informatiile despre client 
                Client client_info_actualizat;
                MPI_Recv(&client_info_actualizat, sizeof(Client), MPI_CHAR, rank_client, 0, MPI_COMM_WORLD, &status);

                // cautam clientul 
                int client_index_nou;
                client_index_nou = find_client(swarm, rank_client, update_file_index);

                if(client_index_nou > -1) {
                    // adaugam doar segmentele deoaerece clientul exista deja
                    int aux_nr_bucati = swarm[update_file_index].file.nr_bucati_fisier;
                    for(int j = 0; j < aux_nr_bucati; j++) {
                        swarm[update_file_index].client_info[client_index_nou].segment_exit[j] = client_info_actualizat.segment_exit[j];
                    }
                } else {
                    // daca clientul nu avea inainte fisierul acum il adaugam ca 
                    // detinator al fisierului pentru segmentele pe care le are la momentul actual
                    swarm[update_file_index].client_info[swarm[update_file_index].nr_clienti].client_id = rank_client;

                    int aux_nr_bucati = swarm[update_file_index].file.nr_bucati_fisier;
                    for(int i = 0; i < aux_nr_bucati; i++) {
                        swarm[update_file_index].client_info[swarm[update_file_index].nr_clienti].segment_exit[i] = client_info_actualizat.segment_exit[i];
                    }

                    swarm[update_file_index].nr_clienti = swarm[update_file_index].nr_clienti + 1; // actualizam nr de clienti
                }

                // TRIMITEM INFORMATIILE ACTUALIZATE DESPRE FISIER
                MPI_Send(&swarm[update_file_index], sizeof(Swarm), MPI_CHAR, rank_client, 0, MPI_COMM_WORLD);

            } else {
                printf("Fisierul %s nu exista in swarm\n", nume_fisier_actualizat);
            }

        }  else if (strncmp(mesaj_client, "DONE_FILE", 9) == 0) {
            //------------- FACEM CLIENTUL, CARE A DESCARCAT FISIERUL, SEED ------------

            //primim numele fisierului terminat de descarcat de la thread-ul de download
            char finished_nume_fisier[MAX_FILENAME];
            MPI_Recv(finished_nume_fisier, MAX_FILENAME, MPI_CHAR, rank_client, 0, MPI_COMM_WORLD, &status);

            // cautam fisierul
            int finish_file_index;
            finish_file_index = find_file(swarm, finished_nume_fisier, nr_fisiere_swarm);

            //cautam client
            int finish_client_index;
            finish_client_index = find_client(swarm, rank_client, finish_file_index);

            if (finish_client_index > -1) {
                swarm[finish_file_index].client_info[finish_client_index].is_seed = 1; //SEED   
            }

        } else if (strncmp(mesaj_client, "EXIT", 4) == 0) {
            // -------- AM TERMINAT DE DESCARCAT TOATE FISIERELE ------------
            cnt_finished--;
            if (cnt_finished == 0) {
                int aux = 1;
                while(aux < numtasks) {
                    // trimitem mesaj de inchidere la upload thread
                    MPI_Send("CLOSE", 6, MPI_CHAR, aux, 1, MPI_COMM_WORLD);
                    aux++;
                }
            }

        }

        if (cnt_finished == 0) {
            printf("TRACKER-UL S-A OPRIT\n");
            break;
        }
    }
}

void peer(int numtasks, int rank) {
    // citeste datele din fisierul de intrare
    Data_Info general_info;
    general_info = read_file_input(rank);

    pthread_t download_thread;
    pthread_t upload_thread;
    void *status;
    int r;

    r = pthread_create(&download_thread, NULL, download_thread_func, (void *) &general_info);
    if (r) {
        printf("Eroare la crearea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_create(&upload_thread, NULL, upload_thread_func, (void *) &general_info);
    if (r) {
        printf("Eroare la crearea thread-ului de upload\n");
        exit(-1);
    }

    r = pthread_join(download_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_join(upload_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de upload\n");
        exit(-1);
    }
}
 
int main (int argc, char *argv[]) {
    int numtasks, rank;
 
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_MULTIPLE) {
        fprintf(stderr, "MPI nu are suport pentru multi-threading\n");
        exit(-1);
    }
    MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == TRACKER_RANK) {
        tracker(numtasks, rank);
    } else {
        peer(numtasks, rank);
    }

    MPI_Finalize();
}
