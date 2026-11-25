# Lesson 6.5 – Multicore, ISRs, Shared Resources
 
## Task
 
### Hardware
 
- ESP32-S3 DevKit
- **Button 1**: simulates an optical sensor that counts parts
- **Button 2** (or **BOOT**): emergency stop
- **LED**: alarm indicator
- **A free ADC pin**, or a real sensor (for example, a temperature sensor)
### Operating logic
 
1. **Global part counter**
   - Button 1 triggers an ISR that increments a global counter by 1.
   - Other tasks read the counter.
2. **System report**
   - There is a single global character array (string) used as the report buffer.
   - **Task 1 (Core 0):** once per second, reads the counter, calculates the conveyor speed (parts/min) and formats the data into the shared report buffer.
   - **Task 2 (Core 1):** once per second, reads the ADC and appends the value to the same buffer.
3. **Emergency stop**
   - Button 2 triggers an ISR.
   - **Task 3** has the highest priority. Normally it sleeps and uses no CPU time.
   - When Button 2 is pressed, Task 3 wakes up immediately, turns on the LED, safely writes `SYSTEM HALTED!` to the buffer and blocks any further conveyor operation.
 
