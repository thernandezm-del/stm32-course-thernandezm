/**
 ******************************************************************************
 * @file           : main.c
 * @author         : Tomas Hernandez Maya
 * @brief          : Main program body
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2025 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */
#include <stdint.h>
#include <stm32f411xe.h>
#include <stm32f4xx.h>

/* Declaración de estados y variables */

/*
 * Estados del multiplexado del display.
 * Cada estado identifica el dígito que se mostrará en un turno de refresco.
 */
enum estado_display
{
	UNIDADES,
	DECENAS,
	CENTENAS,
	MILLARES
};

/*
 * Próximo dígito que mostrará cambio_digitos(), comenzando por unidades.
 * Conserva su valor entre llamadas y solo se utiliza desde el programa principal.
 */
enum estado_display digito_actual = UNIDADES;

/*
 * Solicitud de incremento: la ISR de EXTI la activa ante un evento de PB6.
 * check_flags() la limpia y aumenta counter.
 * Es volatile porque se comparte entre la ISR y el programa principal.
 */
volatile uint8_t uflag = 0;

/*
 * Solicitud de decremento: la ISR de EXTI la activa ante un evento de PB5.
 * check_flags() la limpia y reduce counter.
 * Es volatile porque se comparte entre la ISR y el programa principal.
 */
volatile uint8_t dflag = 0;

/*
 * Solicitud de refresco: TIM4_IRQHandler la activa y cambio_digitos()
 * la limpia antes de mostrar el siguiente dígito.
 * Es volatile porque se comparte entre la ISR y el programa principal.
 * Las tres banderas indican un evento pendiente; no acumulan varios eventos.
 */
volatile uint8_t digito_flag = 0;

/*
 * Contador sin signo de 16 bits, actualizado por check_flags().
 * Su rango es 0 a 65535: al incrementar 65535 vuelve a 0 y al restar de 0
 * pasa a 65535. Solo se utiliza desde main() y sus funciones.
 */
uint16_t counter = 0;
/*
 * Cifras decimales calculadas por digit_manager(), cada una entre 0 y 9.
 * Representan las últimas cuatro cifras de counter: 65535 se muestra como 5535.
 * Solo se utilizan desde el programa principal; no necesitan volatile.
 */
uint8_t unidades = 0; /* Cifra de unidades */
uint8_t decenas = 0;  /* Cifra de decenas */
uint8_t centenas = 0; /* Cifra de centenas */
uint8_t miles = 0;    /* Cifra de millares */

/* Prototipos de funciones */

/* Configura y arranca TIM10 para LED_OK y TIM4 para el refresco del display. */
void init_timers(void);

/* Prepara los GPIO de LED_OK, las fotocompuertas y el display. */
void init_gpio(void);

/* Conecta PB5 y PB6 a EXTI y habilita la detección de flancos de subida. */
void init_EXTI(void);

/* Atiende las banderas de las fotocompuertas y actualiza counter. */
void check_flags(void);

/* Extrae las últimas cuatro cifras decimales de counter en sus variables. */
void digit_manager(void);

/*
 * Si hay un refresco pendiente, apaga los selectores, prepara el número
 * y habilita un solo dígito. Guarda el siguiente estado sin esperar al timer.
 */
void cambio_digitos(void);

/*
 * Prepara los segmentos para numero, entre 0 y 9; otros valores los apagan.
 * Debe llamarse con todos los selectores apagados para evitar imágenes falsas.
 * No habilita ningún dígito ni devuelve un valor.
 */
void display_SetNumber(uint8_t numero);


/*
 * Programa principal.
 * Inicializa los periféricos una sola vez y luego atiende continuamente
 * los eventos de las fotocompuertas y el refresco del display.
 */
int main(void)
{
    init_timers(); /* Configurar y arrancar TIM10 y TIM4. */
    init_gpio();   /* Preparar LED_OK, entradas y salidas del display. */
    init_EXTI();   /* Habilitar los eventos de las fotocompuertas. */

    /*
     * Cada vuelta procesa los eventos pendientes y actualiza las cifras.
     * El display solo avanza un turno cuando la bandera de TIM4 está activa.
     */
    while (1)
    {
        /* Aplicar al contador las solicitudes de incremento y decremento. */
        check_flags();

        /* Calcular las cuatro cifras a partir del contador actualizado. */
        digit_manager();

        /* Atender un refresco pendiente y regresar sin esperar otro evento. */
        cambio_digitos();
    }
}
/* Inicialización de periféricos */

/*
 * Configura y arranca los timers dedicados a LED_OK y al display.
 * Los cálculos siguientes suponen un reloj de 16 MHz en ambos timers:
 * T_evento = (PSC + 1) * (ARR + 1) / frecuencia_del_timer.
 * Estos períodos describen el régimen periódico, una vez cargadas las precargas.
 */
void init_timers(void)
{
	/*
	 * TIM10: su ISR alterna LED_OK en cada evento, cada 62,5 ms.
	 * Dos eventos completan un ciclo encendido/apagado: 125 ms, equivalente a 8 Hz.
	 */
	RCC->APB2ENR |= RCC_APB2ENR_TIM10EN; /* Habilitar el reloj en APB2. */
	TIM10->CR1 |= TIM_CR1_ARPE; /* Aplicar cambios de ARR mediante precarga. */
	TIM10->DIER |= TIM_DIER_UIE; /* Permitir solicitudes de interrupción por actualización. */
	TIM10->PSC = 1000-1; /* Dividir entre 1000: tick de 62,5 us a 16 MHz. */
	TIM10->ARR = 1000-1; /* Contar 1000 ticks por evento: 62,5 ms. */
	NVIC_EnableIRQ(TIM1_UP_TIM10_IRQn); /* Habilitar el vector compartido de TIM10. */
	TIM10->CR1 |= TIM_CR1_CEN; /* Arrancar después de configurar el timer. */

	/*
	 * TIM4: un evento cada 3,125 ms solicita avanzar un dígito.
	 * 320 eventos/s equivalen a 80 recorridos/s de los cuatro dígitos,
	 * siempre que el programa principal atienda cada solicitud a tiempo.
	 */
	RCC->APB1ENR |= RCC_APB1ENR_TIM4EN; /* Habilitar el reloj en APB1. */
	TIM4->CR1 |= TIM_CR1_ARPE; /* Aplicar cambios de ARR mediante precarga. */
	TIM4->DIER |= TIM_DIER_UIE; /* Permitir solicitudes de interrupción por actualización. */
	TIM4->PSC = 1000-1; /* Dividir entre 1000: contador a 16 kHz. */
	TIM4->ARR = 50-1; /* Contar 50 ticks por evento: 3,125 ms. */
	NVIC_EnableIRQ(TIM4_IRQn); /* Permitir que la CPU atienda la interrupción de TIM4. */
	TIM4->CR1 |= TIM_CR1_CEN; /* Arrancar después de configurar el timer. */
}

/*
 * Configura LED_OK en PH1, las entradas de sensores en PB5/PB6
 * y las salidas del display de ánodo común en GPIOC.
 * Los segmentos y selectores PNP se activan en bajo y se apagan en alto.
 */
void init_gpio(void)
{
	/* LED_OK: salida push-pull; baja velocidad de transición es suficiente. */
	RCC->AHB1ENR |= RCC_AHB1ENR_GPIOHEN;
	GPIOH->MODER &= ~GPIO_MODER_MODER1; /* Limpiar el campo completo del modo. */
	GPIOH->MODER |= GPIO_MODER_MODER1_0; /* Seleccionar salida: 01. */
	GPIOH->OTYPER &= ~GPIO_OTYPER_OT_1; /* Seleccionar push-pull. */
	GPIOH->OSPEEDR &= ~GPIO_OSPEEDER_OSPEEDR1; /* Seleccionar baja velocidad. */

	/*
	 * Fotocompuertas: PB5 resta y PB6 suma.
	 * Cada receptor tiene un pull-up externo de 10 kohm a 3,3 V,
	 * por lo que no se habilitan resistencias internas.
	 * La selección del flanco se hace en EXTI, no en MODER.
	 */
	RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
	GPIOB->MODER &= ~GPIO_MODER_MODER5; /* PB5 como entrada: 00. */
	GPIOB->PUPDR &= ~GPIO_PUPDR_PUPD5; /* PB5 sin pull interno. */
	GPIOB->MODER &= ~GPIO_MODER_MODER6; /* PB6 como entrada: 00. */
	GPIOB->PUPDR &= ~GPIO_PUPDR_PUPD6; /* PB6 sin pull interno. */

	/*
	 * Display: a=PC1, b=PC2, c=PC8, d=PC6, e=PC5, f=PC0, g=PC9.
	 * Selectores: unidades=PC13, decenas=PC10, centenas=PC12, millares=PC11.
	 */
	RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN;
	/* Dejar los pines como entradas mientras se preparan. */
	GPIOC->MODER &= ~GPIO_MODER_MODER1;
	GPIOC->MODER &= ~GPIO_MODER_MODER2;
	GPIOC->MODER &= ~GPIO_MODER_MODER8;
	GPIOC->MODER &= ~GPIO_MODER_MODER6;
	GPIOC->MODER &= ~GPIO_MODER_MODER5;
	GPIOC->MODER &= ~GPIO_MODER_MODER0;
	GPIOC->MODER &= ~GPIO_MODER_MODER9;
	GPIOC->MODER &= ~GPIO_MODER_MODER10;
	GPIOC->MODER &= ~GPIO_MODER_MODER11;
	GPIOC->MODER &= ~GPIO_MODER_MODER12;
	GPIOC->MODER &= ~GPIO_MODER_MODER13;

	/* Seleccionar push-pull */
	GPIOC->OTYPER &= ~GPIO_OTYPER_OT_1;
	GPIOC->OTYPER &= ~GPIO_OTYPER_OT_2;
	GPIOC->OTYPER &= ~GPIO_OTYPER_OT_8;
	GPIOC->OTYPER &= ~GPIO_OTYPER_OT_6;
	GPIOC->OTYPER &= ~GPIO_OTYPER_OT_5;
	GPIOC->OTYPER &= ~GPIO_OTYPER_OT_0;
	GPIOC->OTYPER &= ~GPIO_OTYPER_OT_9;
	GPIOC->OTYPER &= ~GPIO_OTYPER_OT_10;
	GPIOC->OTYPER &= ~GPIO_OTYPER_OT_11;
	GPIOC->OTYPER &= ~GPIO_OTYPER_OT_12;
	GPIOC->OTYPER &= ~GPIO_OTYPER_OT_13;

	/* Preparar el nivel alto antes de habilitar las salidas: PNP apagados. */
	GPIOC->BSRR = GPIO_BSRR_BS10;
	GPIOC->BSRR = GPIO_BSRR_BS11;
	GPIOC->BSRR = GPIO_BSRR_BS12;
	GPIOC->BSRR = GPIO_BSRR_BS13;

	/* Preparar segmentos apagados: nivel alto */
	GPIOC->BSRR = GPIO_BSRR_BS1; /* a */
	GPIOC->BSRR = GPIO_BSRR_BS2; /* b */
	GPIOC->BSRR = GPIO_BSRR_BS8; /* c */
	GPIOC->BSRR = GPIO_BSRR_BS6; /* d */
	GPIOC->BSRR = GPIO_BSRR_BS5; /* e */
	GPIOC->BSRR = GPIO_BSRR_BS0; /* f */
	GPIOC->BSRR = GPIO_BSRR_BS9; /* g */

	/* Habilitar salidas de los selectores, ya apagados */
	GPIOC->MODER |= GPIO_MODER_MODER10_0;
	GPIOC->MODER |= GPIO_MODER_MODER11_0;
	GPIOC->MODER |= GPIO_MODER_MODER12_0;
	GPIOC->MODER |= GPIO_MODER_MODER13_0;

	/* Habilitar salidas de los segmentos, ya apagados */
	GPIOC->MODER |= GPIO_MODER_MODER1_0;
	GPIOC->MODER |= GPIO_MODER_MODER2_0;
	GPIOC->MODER |= GPIO_MODER_MODER8_0;
	GPIOC->MODER |= GPIO_MODER_MODER6_0;
	GPIOC->MODER |= GPIO_MODER_MODER5_0;
	GPIOC->MODER |= GPIO_MODER_MODER0_0;
	GPIOC->MODER |= GPIO_MODER_MODER9_0;

}

/*
 * Selecciona el puerto B para EXTI5 y EXTI6 y detecta flancos de subida.
 * En el circuito probado, bloquear el haz cambia la señal de bajo a alto.
 * Las dos líneas comparten EXTI9_5_IRQHandler, que consulta PR para distinguirlas.
 */
void init_EXTI(void)
{
	RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN; /* Permitir configurar el enrutamiento GPIO-EXTI. */

	/* EXTI5: un evento de PB5 solicita decrementar el contador. */
	SYSCFG->EXTICR[1] &= ~SYSCFG_EXTICR2_EXTI5; /* Borrar la selección anterior del puerto. */
	SYSCFG->EXTICR[1] |= SYSCFG_EXTICR2_EXTI5_PB; /* Conectar PB5 a EXTI5. */
	EXTI->FTSR |= EXTI_FTSR_TR5; /* Deshabilitar detección de bajada. */
	EXTI->RTSR &= ~EXTI_RTSR_TR5; /* Habilitar detección de subida. */
	EXTI->IMR |= EXTI_IMR_MR5; /* Permitir la solicitud de interrupción de EXTI5. */

	/* EXTI6: un evento de PB6 solicita incrementar el contador. */
	SYSCFG->EXTICR[1] &= ~SYSCFG_EXTICR2_EXTI6;
	SYSCFG->EXTICR[1] |= SYSCFG_EXTICR2_EXTI6_PB; /* Conectar PB6 a EXTI6. */
	EXTI->FTSR &= ~EXTI_FTSR_TR6; /* Deshabilitar detección de bajada. */
	EXTI->RTSR |= EXTI_RTSR_TR6; /* Habilitar detección de subida. */
	EXTI->IMR |= EXTI_IMR_MR6; /* Permitir la solicitud de interrupción de EXTI6. */

	NVIC_EnableIRQ(EXTI2_IRQn); /* Habilitación independiente; no corresponde a PB5/PB6. */
	NVIC_EnableIRQ(EXTI9_5_IRQn); /* Habilitar el grupo compartido por EXTI5 y EXTI6. */
}
/* Fin de las inicializaciones */

/* Rutinas de servicio de interrupción: llamadas por el hardware, no por main(). */

/*
 * Atiende el evento de actualización de TIM10 dentro de su vector compartido.
 * Esta implementación alterna LED_OK directamente en la ISR: no usa una bandera.
 * Por eso LED_OK indica actividad de TIM10, pero no garantiza que main() avance.
 */
void TIM1_UP_TIM10_IRQHandler(void)
{
	if (TIM10->SR & TIM_SR_UIF) /* Consultar UIF sin modificar el registro. */
	{
		TIM10->SR &= ~TIM_SR_UIF; /* Reconocer el evento limpiando UIF. */
		GPIOH->ODR ^= GPIO_ODR_OD1; /* Alternar PH1 una vez por evento atendido. */
	}
}

/*
 * Handler compartido por las líneas EXTI5 a EXTI9; aquí se atienden PB5 y PB6.
 * Las comprobaciones son independientes para atender ambas si están pendientes.
 * PR se limpia escribiendo 1 en el bit elegido: los ceros no alteran otros bits.
 * Se usa asignación directa para no limpiar accidentalmente otros pendientes.
 */
void EXTI9_5_IRQHandler(void)
{
	if (EXTI->PR & EXTI_PR_PR5)
	{
		EXTI->PR = EXTI_PR_PR5; /* Limpiar únicamente el pendiente de EXTI5. */
		dflag = 1; /* Solicitar a main() un decremento. */
	}

	if (EXTI->PR & EXTI_PR_PR6)
	{
		EXTI->PR = EXTI_PR_PR6; /* Limpiar únicamente el pendiente de EXTI6. */
		uflag = 1; /* Solicitar a main() un incremento. */
	}
}

/*
 * Reconoce la actualización de TIM4 y solicita un turno de refresco.
 * La selección de segmentos y dígitos se realiza después desde main().
 */
void TIM4_IRQHandler(void)
{
	if (TIM4->SR & TIM_SR_UIF)
	{
		TIM4->SR &= ~TIM_SR_UIF;
		digito_flag = 1;
	}
}

/* Funciones ejecutadas desde el programa principal */

/*
 * Procesa cada solicitud de las fotocompuertas sin esperar nuevos eventos.
 * Limpia la bandera antes de actualizar counter para no borrarla después
 * de que una nueva interrupción la active durante esa actualización.
 * Una bandera no cuenta eventos: varios avisos anteriores pueden quedar en uno,
 * y comprobarla y limpiarla tampoco constituye una operación indivisible.
 */
void check_flags(void)
{
	if (uflag == 1)
	{
		uflag = 0;
		counter += 1;
	}
	if (dflag == 1)
	{
		dflag = 0;
		counter -= 1;
	}
}

/*
 * La división entera desplaza las cifras decimales; el módulo 10 extrae una.
 * Por ejemplo, con counter = 1234, (counter / 100) % 10 produce 2.
 * Los valores mayores que 9999 conservan solo sus últimas cuatro cifras.
 */
void digit_manager(void)
{
	unidades = counter % 10;
	decenas = (counter / 10) % 10;
	centenas = (counter / 100) % 10;
	miles = (counter / 1000) % 10;
}

/*
 * Ejecuta un solo turno de multiplexado cuando TIM4 deja un aviso pendiente.
 * Orden: apagar selectores, preparar segmentos y habilitar el dígito elegido.
 * El apagado previo evita mostrar el patrón nuevo en el dígito anterior.
 * Guarda el próximo estado y retorna; el GPIO mantiene el dígito encendido
 * hasta el siguiente turno. Si no hay bandera, retorna sin cambiar las salidas.
 */
void cambio_digitos(void)
{
	if (digito_flag == 1)
	{
		digito_flag = 0;

		/* Un nivel alto apaga cada PNP de selección. */
		GPIOC->BSRR = GPIO_BSRR_BS10;
		GPIOC->BSRR = GPIO_BSRR_BS11;
		GPIOC->BSRR = GPIO_BSRR_BS12;
		GPIOC->BSRR = GPIO_BSRR_BS13;

		switch (digito_actual)
		{
			case UNIDADES:
				/* Mostrar unidades en PC13 y preparar el turno de decenas. */
				display_SetNumber(unidades);
				GPIOC->BSRR = GPIO_BSRR_BR13;
				digito_actual = DECENAS;
				break;

			case DECENAS:
				/* Mostrar decenas en PC10 y preparar el turno de centenas. */
				display_SetNumber(decenas);
				GPIOC->BSRR = GPIO_BSRR_BR10;
				digito_actual = CENTENAS;
				break;

			case CENTENAS:
				/* Mostrar centenas en PC12 y preparar el turno de millares. */
				display_SetNumber(centenas);
				GPIOC->BSRR = GPIO_BSRR_BR12;
				digito_actual = MILLARES;
				break;

			case MILLARES:
				/* Mostrar millares en PC11 y volver al turno de unidades. */
				display_SetNumber(miles);
				GPIOC->BSRR = GPIO_BSRR_BR11;
				digito_actual = UNIDADES;
				break;
		}
	}
}

/*
 * Convierte numero en el patrón de los siete segmentos del display.
 * Cada caso define todos los segmentos, sin depender del patrón anterior.
 * Ánodo común: BR pone el cátodo en bajo y enciende; BS lo pone en alto y apaga.
 * El llamador mantiene apagados los selectores mientras se prepara el patrón.
 * Un valor fuera de 0 a 9 deja los siete segmentos apagados mediante default.
 */
void display_SetNumber(uint8_t numero)
{
	switch (numero)
	{
		case 0:
			GPIOC->BSRR = GPIO_BSRR_BR1; /* Encender a */
			GPIOC->BSRR = GPIO_BSRR_BR2; /* Encender b */
			GPIOC->BSRR = GPIO_BSRR_BR8; /* Encender c */
			GPIOC->BSRR = GPIO_BSRR_BR6; /* Encender d */
			GPIOC->BSRR = GPIO_BSRR_BR5; /* Encender e */
			GPIOC->BSRR = GPIO_BSRR_BR0; /* Encender f */
			GPIOC->BSRR = GPIO_BSRR_BS9; /* Apagar g */
			break;

		case 1:
			GPIOC->BSRR = GPIO_BSRR_BS1; /* Apagar a */
			GPIOC->BSRR = GPIO_BSRR_BR2; /* Encender b */
			GPIOC->BSRR = GPIO_BSRR_BR8; /* Encender c */
			GPIOC->BSRR = GPIO_BSRR_BS6; /* Apagar d */
			GPIOC->BSRR = GPIO_BSRR_BS5; /* Apagar e */
			GPIOC->BSRR = GPIO_BSRR_BS0; /* Apagar f */
			GPIOC->BSRR = GPIO_BSRR_BS9; /* Apagar g */
			break;

		case 2:
			GPIOC->BSRR = GPIO_BSRR_BR1; /* Encender a */
			GPIOC->BSRR = GPIO_BSRR_BR2; /* Encender b */
			GPIOC->BSRR = GPIO_BSRR_BS8; /* Apagar c */
			GPIOC->BSRR = GPIO_BSRR_BR6; /* Encender d */
			GPIOC->BSRR = GPIO_BSRR_BR5; /* Encender e */
			GPIOC->BSRR = GPIO_BSRR_BS0; /* Apagar f */
			GPIOC->BSRR = GPIO_BSRR_BR9; /* Encender g */
			break;

		case 3:
			GPIOC->BSRR = GPIO_BSRR_BR1; /* Encender a */
			GPIOC->BSRR = GPIO_BSRR_BR2; /* Encender b */
			GPIOC->BSRR = GPIO_BSRR_BR8; /* Encender c */
			GPIOC->BSRR = GPIO_BSRR_BR6; /* Encender d */
			GPIOC->BSRR = GPIO_BSRR_BS5; /* Apagar e */
			GPIOC->BSRR = GPIO_BSRR_BS0; /* Apagar f */
			GPIOC->BSRR = GPIO_BSRR_BR9; /* Encender g */
			break;

		case 4:
			GPIOC->BSRR = GPIO_BSRR_BS1; /* Apagar a */
			GPIOC->BSRR = GPIO_BSRR_BR2; /* Encender b */
			GPIOC->BSRR = GPIO_BSRR_BR8; /* Encender c */
			GPIOC->BSRR = GPIO_BSRR_BS6; /* Apagar d */
			GPIOC->BSRR = GPIO_BSRR_BS5; /* Apagar e */
			GPIOC->BSRR = GPIO_BSRR_BR0; /* Encender f */
			GPIOC->BSRR = GPIO_BSRR_BR9; /* Encender g */
			break;

		case 5:
			GPIOC->BSRR = GPIO_BSRR_BR1; /* Encender a */
			GPIOC->BSRR = GPIO_BSRR_BS2; /* Apagar b */
			GPIOC->BSRR = GPIO_BSRR_BR8; /* Encender c */
			GPIOC->BSRR = GPIO_BSRR_BR6; /* Encender d */
			GPIOC->BSRR = GPIO_BSRR_BS5; /* Apagar e */
			GPIOC->BSRR = GPIO_BSRR_BR0; /* Encender f */
			GPIOC->BSRR = GPIO_BSRR_BR9; /* Encender g */
			break;

		case 6:
			GPIOC->BSRR = GPIO_BSRR_BR1; /* Encender a */
			GPIOC->BSRR = GPIO_BSRR_BS2; /* Apagar b */
			GPIOC->BSRR = GPIO_BSRR_BR8; /* Encender c */
			GPIOC->BSRR = GPIO_BSRR_BR6; /* Encender d */
			GPIOC->BSRR = GPIO_BSRR_BR5; /* Encender e */
			GPIOC->BSRR = GPIO_BSRR_BR0; /* Encender f */
			GPIOC->BSRR = GPIO_BSRR_BR9; /* Encender g */
			break;

		case 7:
			GPIOC->BSRR = GPIO_BSRR_BR1; /* Encender a */
			GPIOC->BSRR = GPIO_BSRR_BR2; /* Encender b */
			GPIOC->BSRR = GPIO_BSRR_BR8; /* Encender c */
			GPIOC->BSRR = GPIO_BSRR_BS6; /* Apagar d */
			GPIOC->BSRR = GPIO_BSRR_BS5; /* Apagar e */
			GPIOC->BSRR = GPIO_BSRR_BS0; /* Apagar f */
			GPIOC->BSRR = GPIO_BSRR_BS9; /* Apagar g */
			break;

		case 8:
			GPIOC->BSRR = GPIO_BSRR_BR1; /* Encender a */
			GPIOC->BSRR = GPIO_BSRR_BR2; /* Encender b */
			GPIOC->BSRR = GPIO_BSRR_BR8; /* Encender c */
			GPIOC->BSRR = GPIO_BSRR_BR6; /* Encender d */
			GPIOC->BSRR = GPIO_BSRR_BR5; /* Encender e */
			GPIOC->BSRR = GPIO_BSRR_BR0; /* Encender f */
			GPIOC->BSRR = GPIO_BSRR_BR9; /* Encender g */
			break;

		case 9:
			GPIOC->BSRR = GPIO_BSRR_BR1; /* Encender a */
			GPIOC->BSRR = GPIO_BSRR_BR2; /* Encender b */
			GPIOC->BSRR = GPIO_BSRR_BR8; /* Encender c */
			GPIOC->BSRR = GPIO_BSRR_BR6; /* Encender d */
			GPIOC->BSRR = GPIO_BSRR_BS5; /* Apagar e */
			GPIOC->BSRR = GPIO_BSRR_BR0; /* Encender f */
			GPIOC->BSRR = GPIO_BSRR_BR9; /* Encender g */
			break;

		default:
			GPIOC->BSRR = GPIO_BSRR_BS1; /* Apagar a */
			GPIOC->BSRR = GPIO_BSRR_BS2; /* Apagar b */
			GPIOC->BSRR = GPIO_BSRR_BS8; /* Apagar c */
			GPIOC->BSRR = GPIO_BSRR_BS6; /* Apagar d */
			GPIOC->BSRR = GPIO_BSRR_BS5; /* Apagar e */
			GPIOC->BSRR = GPIO_BSRR_BS0; /* Apagar f */
			GPIOC->BSRR = GPIO_BSRR_BS9; /* Apagar g */
			/* Valor fuera de 0–9: mantener los segmentos apagados */
			break;
	}
}
/* Fin de las funciones */
