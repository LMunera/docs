#include "filosofar.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/sem.h>
#include <sys/wait.h>
#include <sys/shm.h>
#include <string.h>
#include <signal.h>
#include <sys/msg.h>
#include <stdbool.h>
#include <errno.h>

// ===================== CONSTANTES =====================
#define ARGS 			 4	// Número de argumentos esperados (nombre + 3 parámetros)
#define MIN_FILOSOFOS 		 1	// Número mínimo de filósofos permitido
#define MIN_VUELTAS 		 1	// Número mínimo de vueltas permitido
#define MIN_LENTITUD 		 0	// Número mínimo para la lentitud

// Semáforos del puente
#define SEM_PUENTE_DIR1          2
#define SEM_PUENTE_DIR2          3
#define SEM_PUENTE_CAPACIDAD     4
#define SEM_ANTESALA        	 5

// Semáforos de tenedores
#define SEM_TENEDOR1             6
#define SEM_TENEDOR2             7
#define SEM_TENEDOR3             8
#define SEM_TENEDOR4             9
#define SEM_TENEDOR5            10

// Semáforos del comedor
#define SEM_ACCESO_COMEDOR      11	// Controla acceso al comedor
#define SEM_MUTEX_COMEDOR       12	// Protege selección de sitio en comedor
#define SEM_LIBERAR_COMEDOR     13	// Protege liberación de sitio en comedor

// Semáforos del templo
#define SEM_ACCESO_TEMPLO       14	// Controla acceso al templo
#define SEM_MUTEX_TEMPLO        15	// Protege selección de sitio en templo

// Semáforos de movimiento
#define SEM_MOV_COMEDOR         16	// Control movimiento en comedor
#define SEM_MOV_TEMPLO          17	// Control movimiento en templo
#define SEM_MOV_BLOQUEO         18	// Control mensajes de bloqueo
#define SEM_CONTROL_PUENTE      19	// Control acceso al puente

// ===================== ESTRUCTURAS =====================
union semun {
	int val;
	struct semid_ds *buf;
	unsigned short *array;
};

typedef struct recursosIPC {
	int memoria_principal;
	int semaforos;
	int buzones;
} recursosIPC;

struct mensaje {
	long tipo;
};

// ===================== VARIABLES GLOBALES =====================
recursosIPC ipc;                      // Recursos IPC compartidos
int hijos_creados;                    // Contador de procesos hijos creados
int proceso_coordinador;              // PID del proceso principal
int procesos_filosofos[MAXFILOSOFOS]; // Array de PIDs de filósofos
struct DatosSimulaciOn config;        // Configuración de la simulación

// ===================== PROTOTIPOS FUNCIONES =====================
void manejar_senales();
void iniciar_simulacion(int velocidad);
void configurar_semaforos();
void configurar_buzones();
void filosofo_vuelta(int id, int vuelta, int* asientos_comedor, int* asientos_templo, int totalVueltas);
void manejador(int signal);
void wait_semaforo(int id_sem, int indice_sem);
void signal_semaforo(int id_sem, int indice_sem);
void wait_cero_semaforo(int id_sem, int indice_sem);
void limpiar_recursos(int num_filo);
void comer_filosofo(int semaforo, int sitio_mesa_libre, int *zona);
int filosofo_avanzar(int id);

// ===================== OPERACIONES DE SEMÁFOROS =====================
void wait_semaforo(int id_sem, int indice_sem){
	struct sembuf operacion;
	operacion.sem_num = indice_sem;  // Semáforo a bloquear
	operacion.sem_op = -1;           // Decrementa el semáforo (P())
	operacion.sem_flg = 0;           // Sin flags especiales

	if(semop(id_sem, &operacion, 1) == -1){
		perror("Error en wait_semaforo");
		exit(EXIT_FAILURE);
	}
}

void signal_semaforo(int id_sem, int indice_sem){
	struct sembuf operacion;
	operacion.sem_num = indice_sem;  // Semáforo a liberar
	operacion.sem_op = 1;            // Incrementa el semáforo (V())
	operacion.sem_flg = 0;           // Sin flags especiales

	if(semop(id_sem, &operacion, 1) == -1){
		char mensaje[100];
		sprintf(mensaje, "Liberar(%d):%d", indice_sem, semctl(id_sem, indice_sem, GETVAL));
		perror(mensaje);
		exit(EXIT_FAILURE);
	}
}

void wait_cero_semaforo(int id_sem, int indice_sem) {
	struct sembuf operacion;
	operacion.sem_num = indice_sem;  // Índice del semáforo
	operacion.sem_op = 0;            // Esperar a que sea cero
	operacion.sem_flg = 0;           // Sin flags especiales

	if(semop(id_sem, &operacion, 1) == -1) {
		perror("Error en wait_cero_semaforo");
		exit(EXIT_FAILURE);
	}
}

// ===================== VALIDACIÓN DE ARGUMENTOS =====================
bool esNumerico(const char *str) {
	if(str == NULL || *str == '\0') return false;
	char *endptr;
	strtol(str, &endptr, 10);
	return *endptr == '\0' && endptr != str;
}

void uso(const char *name_ejec) {
	fprintf(stderr, "Uso: %s <filósofos 1-%d> <vueltas >0> <lentitud ejecución >=0>\n", 
            name_ejec, MAXFILOSOFOS);
}

void comprobarArgumento(const char *numStr, int *arg, const char *nombre_arg, 
                        int min_val, int max_val, const char *name_ejec) {
	if(!esNumerico(numStr)) {
		uso(name_ejec);
		fprintf(stderr, "Error: Argumento '%s' no es numérico\n", nombre_arg);
		exit(EXIT_FAILURE);
	}

	*arg = atoi(numStr);

	if(*arg < min_val || (max_val != -1 && *arg > max_val)) {
		uso(name_ejec);
		fprintf(stderr, "Error: Argumento '%s' fuera de rango [%d,%d]\n", 
		nombre_arg, min_val, max_val);
		exit(EXIT_FAILURE);
	}
}

// ===================== FUNCIÓN PRINCIPAL =====================
int main(int argc, char *argv[]) {
	int num_filo, lentitud_ejec, total_vueltas;
	int i;

	// Validar numero de argumentos
	if (argc < ARGS) {
		uso(argv[0]);
		fprintf(stderr, "Error: Se requieren mínimo %d argumentos.\n", ARGS-1);
		return EXIT_FAILURE;
	}

	// Validar y convertir argumentos
	comprobarArgumento(argv[1], &num_filo, "filósofos", MIN_FILOSOFOS, MAXFILOSOFOS, argv[0]);
	comprobarArgumento(argv[2], &total_vueltas, "vueltas", MIN_VUELTAS, -1, argv[0]);
	comprobarArgumento(argv[3], &lentitud_ejec, "lentitud", MIN_LENTITUD, -1, argv[0]);

	// Configurar manejo de señales
	proceso_coordinador = getpid();
	manejar_senales();

	// Configurar parámetros de simulación
	struct DatosSimulaciOn config = {
		.maxFilOsofosEnPuente = 2,  // Máx filósofos en puente simultáneamente
		.maxUnaDirecciOnPuente = 0,  // Sin límite de dirección en puente
		.sitiosTemplo = 3,          // Capacidad del templo
		.nTenedores = 5             // Número de tenedores
	};

	// Inicializar recursos IPC
	ipc.semaforos = -2;
	ipc.buzones = -2;
	ipc.memoria_principal = -2;

	// Configurar semáforos y buzones
	configurar_semaforos();
	configurar_buzones();

	// Reservar memoria compartida
	int tam_memoria = FI_getTamaNoMemoriaCompartida() + (sizeof(int) * 5) + (sizeof(int) * 3);
	if ((ipc.memoria_principal = shmget(IPC_PRIVATE, tam_memoria, 0664 | IPC_CREAT)) == -1) {
		perror("shmget");
		kill(getpid(), SIGINT);
	}

	// Mapear memoria compartida
	int *ptr_mem = (int *)shmat(ipc.memoria_principal, 0, 0);
	if (ptr_mem == (void *)-1) {
		perror("shmat");
		kill(getpid(), SIGINT);
	}

	// Inicializar arrays de asientos
	int *asientos_comedor = (int *)((char *)ptr_mem + FI_getTamaNoMemoriaCompartida());
	int *asientos_templo  = (int *)((char *)ptr_mem + FI_getTamaNoMemoriaCompartida() + (sizeof(int) * 5));
	for (i = 0; i < 5; i++) asientos_comedor[i] = (i < 3) ? (asientos_templo[i] = 0) : 0;

	// Iniciar simulación gráfica
	iniciar_simulacion(lentitud_ejec);

	// Crear procesos filósofos
	for (hijos_creados = 0; hijos_creados < num_filo && getpid() == proceso_coordinador; hijos_creados++) {
		procesos_filosofos[hijos_creados] = fork();
		
		if (procesos_filosofos[hijos_creados] == -1) {
			perror("fork");
			kill(getpid(), SIGINT);
		}
		else if (procesos_filosofos[hijos_creados] == 0) {
			FI_inicioFilOsofo(hijos_creados);
			for (int vuelta = 0; vuelta < total_vueltas; vuelta++){
				filosofo_vuelta(hijos_creados, vuelta, asientos_comedor, asientos_templo, total_vueltas);
			}

			struct mensaje m;
			if (msgrcv(ipc.buzones, &m, sizeof(m) - sizeof(long), hijos_creados - 100, IPC_NOWAIT) > 0) {
				struct mensaje desbloq = { .tipo = m.tipo + 200 };
				msgsnd(ipc.buzones, &desbloq, sizeof(desbloq) - sizeof(long), IPC_NOWAIT);
			}
			
			FI_finFilOsofo();
			shmdt(ptr_mem);
			exit(0);
		}
	}
	
	// Finalizar simulación
	FI_fin();
	
	limpiar_recursos(num_filo);
	
	return EXIT_SUCCESS;
}

void iniciar_simulacion(int velocidad) {
    unsigned long long clave = 41211294392005ULL;
    
    // Configuración de debug (activar/desactivar zonas)
    int debug[] = {
	    1,  // Cigoto
	    1,  // PasilloSalidaTemplo
	    1,  // CaminoTemploPuente
	    1,  // PuenteHaciaComedor
	    1,  // CaminoPuenteComedor
	    1,  // PasilloEntradaComedor
	    1,  // Antesala
	    1,  // EntradaComedor
	    1,  // SillaComedor
	    1,  // SalidaComedor
	    1,  // PasilloSalidaComedor
	    1,  // ExplanadaComedorPuente
	    1,  // PuenteHaciaTemplo
	    1,  // CaminoPuenteTemplo
	    1,  // PasilloEntradaTemplo
	    1,  // EntradaTemplo
	    1,  // asientos_templo
	    1,  // SalidaTemplo
	    1   // Fin
    };

    FI_inicio(velocidad, clave, &config, ipc.semaforos, ipc.memoria_principal, debug);
}

void configurar_semaforos(){
	int total_semaforos = 20;
	
	union semun arg;
	
	if ((ipc.semaforos = semget(IPC_PRIVATE, total_semaforos, IPC_CREAT | 0600)) == -1) {
		perror("semget");
		kill(getpid(), SIGINT);
	}
	
	// Valores iniciales para los semáforos
	int inicializaciones[][2] = {
		{SEM_ACCESO_COMEDOR, 5},     	// 5 sitios en mesa
		{SEM_MUTEX_COMEDOR, 1},     	// Mutex para selección sitio mesa
		{SEM_LIBERAR_COMEDOR, 1},       // Mutex para liberación sitio mesa
		{SEM_ACCESO_TEMPLO, 3},   	// 3 sitios en templo
		{SEM_MUTEX_TEMPLO, 1},   	// Mutex para selección sitio templo
		{SEM_PUENTE_DIR1, 0},   	// Control dirección 1 puente
		{SEM_PUENTE_DIR2, 0},   	// Control dirección 2 puente
		{SEM_PUENTE_CAPACIDAD, 2},   	// Capacidad puente (2 filósofos)
		{SEM_MOV_COMEDOR , 1},          // Mutex movimiento en comedor
		{SEM_MOV_TEMPLO, 1},          	// Mutex movimiento en templo
		{SEM_MOV_BLOQUEO, 1},          	// Mutex para mensajes de bloqueo
		{SEM_ANTESALA, 4},  		// Capacidad antesala (4 filósofos)
		{SEM_TENEDOR1, 1},       	// Tenedor 1 disponible
		{SEM_TENEDOR2, 1},       	// Tenedor 2 disponible
		{SEM_TENEDOR3, 1},       	// Tenedor 3 disponible
		{SEM_TENEDOR4, 1},       	// Tenedor 4 disponible
		{SEM_TENEDOR5, 1},       	// Tenedor 5 disponible
		{SEM_CONTROL_PUENTE, 1}         // Mutex para control de puente
	};
	
	// Inicializar todos los semáforos
	for (int i = 0; i < sizeof(inicializaciones)/sizeof(inicializaciones[0]); i++) {
		arg.val = inicializaciones[i][1];
		if (semctl(ipc.semaforos, inicializaciones[i][0], SETVAL, arg) == -1) {
			perror("semctl");
			kill(getpid(), SIGINT);
		}
	}
}

void configurar_buzones(){
	if ((ipc.buzones = msgget(IPC_PRIVATE, 0666 | IPC_CREAT)) == -1) {
		perror("msgget");
		kill(getpid(), SIGINT);
	}
}

/* MANEJADOR DE SEÑALES */
void manejar_senales() {
	struct sigaction sa;
	sa.sa_handler = manejador;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;

	// Configurar SIGINT (Ctrl+C)
	if (sigaction(SIGINT, &sa, NULL) == -1) {
		perror("sigaction SIGINT");
		kill(getpid(), SIGINT);
	}

	// Configurar SIGTERM
	if(sigaction(SIGTERM, &sa, NULL) == -1) {
		perror("sigaction SIGTERM");
		limpiar_recursos(hijos_creados);
		exit(EXIT_FAILURE);
	}

	// Ignorar SIGCHLD para evitar procesos zombi
	sa.sa_handler = SIG_IGN;
	if(sigaction(SIGCHLD, &sa, NULL) == -1) {
		perror("sigaction SIGCHLD");
		limpiar_recursos(hijos_creados);
		exit(EXIT_FAILURE);
	}
}

void limpiar_recursos(int num_filo) {
	// Esperar a que todos los filósofos terminen
	for(int i = 0; i < num_filo; i++){
		if(procesos_filosofos[i] > 0){
			waitpid(procesos_filosofos[i], NULL, 0);
		}
	}
	
	// Eliminar conjunto de semáforos
	if(ipc.semaforos != -2){
		if(semctl(ipc.semaforos, 0, IPC_RMID) == -1){
			perror("Error eliminando semáforos");
		}
		ipc.semaforos = -2;
	}
	
	// Liberar memoria compartida
	if(ipc.memoria_principal != -2){
		if(shmctl(ipc.memoria_principal, IPC_RMID, NULL) == -1){
			perror("Error eliminando memoria compartida");
		}
		ipc.memoria_principal = -2;
	}
	
	// Eliminar buzones
	if (ipc.buzones != -2){
		struct mensaje m;
		int result;
		do{
			result = msgrcv(ipc.buzones, &m, sizeof(m) - sizeof(long), 0, IPC_NOWAIT);
			if(result == -1 && errno != ENOMSG){
				perror("Error al vaciar buzón");
				break;
			}
		}while(result > 0);
		msgctl(ipc.buzones, IPC_RMID, NULL);
		ipc.buzones = -2;
	}
	
	// Finalizar simulación
	FI_fin();
}

void manejador(int sig) {
	if (getpid() == proceso_coordinador) {
		// Terminar todos los procesos hijos
		for (int i = 0; i < hijos_creados; i++){
			if (procesos_filosofos[i] > 0){
				kill(procesos_filosofos[i], SIGKILL);
			}
		}
		
		limpiar_recursos(hijos_creados);
	}
	
	system("clear");
	exit(EXIT_SUCCESS);
}

// ===================== FUNCIÓN PARA MOVER AL FILOSOFO =====================
int filosofo_avanzar(int id) {
	int bloqueo = FI_puedoAndar(); // Verificar si está bloqueado
	int buzonid = ipc.buzones; 
	int semaforo = ipc.semaforos;

	// Manejar bloqueos mediante mensajes
	while(bloqueo != 100) {
		// Sección crítica para enviar/recepcionar mensajes
		wait_semaforo(semaforo, SEM_MOV_BLOQUEO);
		struct mensaje bloq;
		bloq.tipo = bloqueo + 100;
		msgsnd(buzonid, &bloq, sizeof(bloq) - sizeof(long), 0);
		struct mensaje pesado;
		msgrcv(buzonid,&pesado,sizeof(pesado) - sizeof(long), id - 200, 0);
		bloqueo = FI_puedoAndar();
		signal_semaforo(semaforo, SEM_MOV_BLOQUEO);
	}

	if(bloqueo == 100) {
		// Sección crítica para desbloqueo
		wait_semaforo(semaforo, SEM_MOV_BLOQUEO);
		struct mensaje bloquear_paso;

		// Verificar si hay mensaje de bloqueo pendiente
		if(msgrcv(buzonid, &bloquear_paso, sizeof(bloqueo) - sizeof(long), 
		     id - 100,IPC_NOWAIT) > 0) {
			struct mensaje desbloquear_paso;
			desbloquear_paso.tipo = bloquear_paso.tipo + 200;
			msgsnd(buzonid, &desbloquear_paso, sizeof(desbloquear_paso) - sizeof(long),0);
		}

		FI_pausaAndar();
		int resultado = FI_andar();
		signal_semaforo(semaforo, SEM_MOV_BLOQUEO);
		return resultado;
	}
}

void avanzar(int *zona, int id){
	*zona = filosofo_avanzar(id);
	
	if(*zona == -1) {
		perror("Error: Avance inválido.");
		kill(getpid(),SIGINT);
	}
}


// ===================== FUNCIÓN PRINCIPAL VUELTA FILÓSOFO =====================
void filosofo_vuelta(int i, int vuelta, int *asientos_mesa, int *asientos_templo, int totalVueltas) {
	int zona; 
	int buzonid = ipc.buzones; 
	int semaforo = ipc.semaforos;
	
	avanzar(&zona, i);	// Paso inicial

	// 1. Caminar desde el inicio hasta el puente
	while(zona == CAMPO) {
		avanzar(&zona, i);
	}

	// 2. Cruzar el puente hacia el comedor
	if(zona == PUENTE) {
		wait_semaforo(semaforo,SEM_CONTROL_PUENTE); // Bloquear acceso al control del puente

		// Esperar a que no vengan filósofos en dirección contraria
		wait_cero_semaforo(semaforo,SEM_PUENTE_DIR1); 
		signal_semaforo(semaforo,SEM_PUENTE_DIR2);		// Indicar nuestra dirección
		wait_semaforo(semaforo,SEM_PUENTE_CAPACIDAD);   	// Esperar espacio en puente
		signal_semaforo(semaforo,SEM_CONTROL_PUENTE);      	// Liberar control

		// Cruzar el puente
		while(zona == PUENTE) {
			avanzar(&zona, i);
		}

		avanzar(&zona, i);	// Paso adicional para salir completamente

		// Liberar recursos del puente
		signal_semaforo(semaforo, SEM_PUENTE_CAPACIDAD); 	// Liberar espacio en puente
		wait_semaforo(semaforo, SEM_PUENTE_DIR2);   		// Dejar de indicar dirección
	}

	// 3. Avanzar hasta la antesala del comedor
	while(zona != ANTESALA) {
		avanzar(&zona, i);
	}

	// 4. Entrar al comedor
	if(zona == ANTESALA) {
		int sitio_mesa_libre;
		wait_semaforo(semaforo, SEM_ANTESALA); // Esperar espacio en antesala

		// Avanzar por la antesala
		while(zona == ANTESALA) {
			avanzar(&zona, i);
		}

		if(zona != ANTESALA) {
			// Intentar entrar al comedor
			wait_semaforo(semaforo,SEM_ACCESO_COMEDOR); 	// Esperar sitio disponible
			wait_semaforo(semaforo,SEM_MUTEX_COMEDOR); 	// Bloquear selección de sitio

			// Buscar sitio libre
			for(int k = 0; k < 5; k++) {
				if(asientos_mesa[k] == 0) {
					sitio_mesa_libre = k;
					break;
				} 
			}

			// Ocupar el sitio
			asientos_mesa[sitio_mesa_libre] = 1;
			signal_semaforo(semaforo,SEM_MUTEX_COMEDOR);
			wait_semaforo(semaforo,SEM_MOV_COMEDOR ); 	// Bloquear movimiento en comedor

			FI_entrarAlComedor(sitio_mesa_libre); // Entrar gráficamente

			avanzar(&zona, i);	// Avanzar un paso

			signal_semaforo(semaforo,SEM_ANTESALA); 	// Liberar espacio en antesala

			// Moverse hasta el asiento
			while(zona == ENTRADACOMEDOR) {
				avanzar(&zona, i);
			}

			signal_semaforo(semaforo,SEM_MOV_COMEDOR ); 	// Liberar movimiento en comedor

			comer_filosofo(semaforo, sitio_mesa_libre, &zona);

			// Liberar el sitio en el comedor
			wait_semaforo(semaforo,SEM_LIBERAR_COMEDOR); 	// Bloquear liberación de sitio
			asientos_mesa[sitio_mesa_libre] = 0; 
			signal_semaforo(semaforo,SEM_LIBERAR_COMEDOR);
			signal_semaforo(semaforo,SEM_ACCESO_COMEDOR); 	// Indicar sitio libre
		}

		// Salir del comedor
		wait_semaforo(semaforo,SEM_MOV_COMEDOR ); 		// Bloquear movimiento para salir
		avanzar(&zona, i);

		// Avanzar hasta salir completamente
		while(zona != 0) {
			avanzar(&zona, i);
		}

		avanzar(&zona, i);	// Avanzar un paso

		signal_semaforo(semaforo,SEM_MOV_COMEDOR ); 		// Liberar movimiento
	}

	// 5. Moverse hacia el puente de regreso
	while(zona != PUENTE) {
		avanzar(&zona, i);
	}

	// 6. Cruzar el puente hacia el templo
	if(zona == PUENTE) {
		wait_semaforo(semaforo,SEM_CONTROL_PUENTE); 		// Bloquear control del puente

		// Esperar a que no vengan filósofos en dirección contraria
		wait_cero_semaforo(semaforo,SEM_PUENTE_DIR2);
		signal_semaforo(semaforo,SEM_PUENTE_DIR1); 		// Indicar nuestra dirección
		wait_semaforo(semaforo,SEM_PUENTE_CAPACIDAD);  		// Esperar espacio en puente
		signal_semaforo(semaforo,SEM_CONTROL_PUENTE);      	// Liberar control

		// Cruzar el puente
		while(zona == PUENTE) {
			avanzar(&zona, i);
		}

		avanzar(&zona, i);	// Paso adicional para salir completamente

		// Liberar recursos del puente
		signal_semaforo(semaforo,SEM_PUENTE_CAPACIDAD); 	// Liberar espacio en puente
		wait_semaforo(semaforo,SEM_PUENTE_DIR1);   		// Dejar de indicar dirección
	}

	// 7. Moverse hacia el templo
	while(zona != TEMPLO) {
		avanzar(&zona, i);
	}

	int sitio_templo_libre; // Índice de sitio en templo
	// 8. Entrar al templo
	if(zona == TEMPLO) {
		wait_semaforo(semaforo,SEM_ACCESO_TEMPLO); 		// Esperar sitio en templo
		wait_semaforo(semaforo,SEM_MUTEX_TEMPLO);  		// Bloquear selección de sitio

		// Buscar sitio libre
		for(sitio_templo_libre = 0 ; sitio_templo_libre < 3 ; sitio_templo_libre++) {
			if(asientos_templo[sitio_templo_libre] == 0) break;
		}

		// Ocupar el sitio
		asientos_templo[sitio_templo_libre] = 1; 
		FI_entrarAlTemplo(sitio_templo_libre);
		signal_semaforo(semaforo,SEM_MUTEX_TEMPLO);
	}

	// Moverse hasta el sitio asignado
	wait_semaforo(semaforo,SEM_MOV_TEMPLO); 			// Bloquear movimiento en templo
	while(zona == TEMPLO) {
		avanzar(&zona, i);
	}
	signal_semaforo(semaforo,SEM_MOV_TEMPLO); 			// Liberar movimiento

	// 9. Meditar
	while(zona == SITIOTEMPLO) {
		zona = FI_meditar();
	}
	
	//Seccion critica para cambiar el valor del puesto en el templo
	wait_semaforo(semaforo,SEM_MUTEX_TEMPLO);
	asientos_templo[sitio_templo_libre] = 0;
	wait_semaforo(semaforo,SEM_MOV_TEMPLO); // Indico que voy a salir, para que no se mueva ningun proceso

	signal_semaforo(semaforo,SEM_MUTEX_TEMPLO);
	signal_semaforo(semaforo,SEM_ACCESO_TEMPLO); // Indico que hay un sitio libre en el templo

	if(vuelta == (totalVueltas - 1)) {
		signal_semaforo(semaforo,SEM_MOV_TEMPLO); // Si es la última vuelta indico que ya se pueden mover, pues mi ejecución acaba
	}else {
		// Avanzar hasta salir completamente del templo
		for(int k = 0; k < 13; k++) {
			avanzar(&zona, i);
		}
		signal_semaforo(semaforo,SEM_MOV_TEMPLO);// Ahora que ha salido, liberar el templo
	}
}

void comer_filosofo(int semaforo, int sitio_mesa_libre, int *zona) {
	switch(sitio_mesa_libre) {
		case 0: // Filósofo 0: tenedores 1 (derecha) y 2 (izquierda)
			wait_semaforo(semaforo, SEM_TENEDOR2);
			FI_cogerTenedor(TENEDORIZQUIERDO);
			wait_semaforo(semaforo, SEM_TENEDOR1);
			FI_cogerTenedor(TENEDORDERECHO);
			break;

		case 1: // Filósofo 1: tenedores 2 (derecha) y 3 (izquierda)
			wait_semaforo(semaforo, SEM_TENEDOR2);
			FI_cogerTenedor(TENEDORDERECHO);
			wait_semaforo(semaforo, SEM_TENEDOR3);
			FI_cogerTenedor(TENEDORIZQUIERDO);
			break;

		case 2: // Filósofo 2: tenedores 3 (derecha) y 4 (izquierda)
			wait_semaforo(semaforo, SEM_TENEDOR3);
			FI_cogerTenedor(TENEDORDERECHO);
			wait_semaforo(semaforo, SEM_TENEDOR4);
			FI_cogerTenedor(TENEDORIZQUIERDO);
			break;

		case 3: // Filósofo 3: tenedores 4 (derecha) y 5 (izquierda)
			wait_semaforo(semaforo, SEM_TENEDOR5);
			FI_cogerTenedor(TENEDORIZQUIERDO);
			wait_semaforo(semaforo, SEM_TENEDOR4);
			FI_cogerTenedor(TENEDORDERECHO);
			break;

		case 4: // Filósofo 4: tenedores 5 (derecha) y 1 (izquierda)
			wait_semaforo(semaforo, SEM_TENEDOR5);
			FI_cogerTenedor(TENEDORDERECHO);
			wait_semaforo(semaforo, SEM_TENEDOR1);
			FI_cogerTenedor(TENEDORIZQUIERDO);
			break;
	}

	while(*zona == SILLACOMEDOR) {
		*zona = FI_comer();
	}

	// Liberar tenedores según el filósofo
	switch(sitio_mesa_libre) {
		case 0:
			FI_dejarTenedor(TENEDORDERECHO);
			signal_semaforo(semaforo, SEM_TENEDOR1);
			FI_dejarTenedor(TENEDORIZQUIERDO);
			signal_semaforo(semaforo, SEM_TENEDOR2);
			break;

		case 1:
			FI_dejarTenedor(TENEDORIZQUIERDO);
			signal_semaforo(semaforo, SEM_TENEDOR3);
			FI_dejarTenedor(TENEDORDERECHO);
			signal_semaforo(semaforo, SEM_TENEDOR2);
			break;

		case 2:
			FI_dejarTenedor(TENEDORIZQUIERDO);
			signal_semaforo(semaforo, SEM_TENEDOR4);
			FI_dejarTenedor(TENEDORDERECHO);
			signal_semaforo(semaforo, SEM_TENEDOR3);
			break;

		case 3:
			FI_dejarTenedor(TENEDORDERECHO);
			signal_semaforo(semaforo, SEM_TENEDOR4);
			FI_dejarTenedor(TENEDORIZQUIERDO);
			signal_semaforo(semaforo, SEM_TENEDOR5);
			break;

		case 4:
			FI_dejarTenedor(TENEDORIZQUIERDO);
			signal_semaforo(semaforo, SEM_TENEDOR1);
			FI_dejarTenedor(TENEDORDERECHO);
			signal_semaforo(semaforo, SEM_TENEDOR5);
			break;
	}
}
