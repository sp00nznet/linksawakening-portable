/**
 * @file audio.c
 * @brief GameBoy Audio Processing Unit implementation
 */

#include "audio.h"
#include "gbrt_debug.h"
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Internal Structures
 * ========================================================================== */

typedef struct {
    /* Registers */
    uint8_t nr10; /* Sweep */
    uint8_t nr11; /* Duty/Length */
    uint8_t nr12; /* Envelope */
    uint8_t nr13; /* Freq Lo */
    uint8_t nr14; /* Freq Hi */
    
    /* Internal State */
    uint16_t freq;       /* Calculated frequency */
    int length_counter;  /* Length counter */
    bool length_enabled;
    int volume;
    int env_timer;
    bool enabled;

    /* Sweep State */
    int sweep_timer;
    uint16_t shadow_freq;
    bool sweep_enabled;
    
    /* Generation */
    uint32_t timer;
    int wave_pos;
} Channel1;

typedef struct {
    uint8_t nr21;
    uint8_t nr22;
    uint8_t nr23;
    uint8_t nr24;
    
    uint16_t freq;
    int length_counter;
    bool length_enabled;
    int volume;
    int env_timer;
    bool enabled;
    
    uint32_t timer;
    int wave_pos;
} Channel2;

typedef struct {
    uint8_t nr30; /* Enable */
    uint8_t nr31; /* Length */
    uint8_t nr32; /* Level */
    uint8_t nr33; /* Freq Lo */
    uint8_t nr34; /* Freq Hi */
    
    uint8_t wave_ram[16];
    
    uint16_t freq;
    int length_counter;
    bool length_enabled;
    bool enabled;
    int wave_pos;

    /* Generation */
    uint32_t timer;
} Channel3;

typedef struct {
    uint8_t nr41; /* Length */
    uint8_t nr42; /* Envelope */
    uint8_t nr43; /* Poly */
    uint8_t nr44; /* Counter/Consecutive */
    
    int length_counter;
    bool length_enabled;
    int volume;
    int env_timer;
    uint16_t lfsr;
    bool enabled;
    uint32_t timer;   /* Cycle accumulator for LFSR clocking */
} Channel4;

typedef struct GBAudio {
    Channel1 ch1;
    Channel2 ch2;
    Channel3 ch3;
    Channel4 ch4;
    
    uint8_t nr50; /* Channel control / ON-OFF / Volume */
    uint8_t nr51; /* Selection of Sound output terminal */
    uint8_t nr52; /* Sound on/off */
    
    /* Frame Sequencer */
    uint32_t fs_timer;
    int fs_step;
    
    /* Sample Generation */
    uint32_t sample_timer;
    uint32_t sample_period; /* Cycles per sample */
    
} GBAudio;

/* ============================================================================
 * Internal Helpers
 * ========================================================================== */

static const uint8_t DUTY_CYCLES[4][8] = {
    {0, 0, 0, 0, 0, 0, 0, 1}, /* 12.5% */
    {1, 0, 0, 0, 0, 0, 0, 1}, /* 25% */
    {1, 0, 0, 0, 0, 1, 1, 1}, /* 50% */
    {0, 1, 1, 1, 1, 1, 1, 0}  /* 75% */
};

/* Noise Divisors: (Divider code) => (Divisor) */
static const int NOISE_DIVISORS[8] = { 8, 16, 32, 48, 64, 80, 96, 112 };

static uint16_t calc_sweep(Channel1* ch) {
    uint16_t new_freq = ch->shadow_freq;
    uint8_t shift = ch->nr10 & 0x07;
    bool sub = (ch->nr10 & 0x08) != 0;
    
    if (shift > 0) {
        uint16_t delta = new_freq >> shift;
        if (sub) new_freq -= delta;
        else     new_freq += delta;
    }
    return new_freq;
}

static void step_envelope(int* timer, int* volume, uint8_t env_reg) {
    if (*timer > 0) {
        (*timer)--;
        if (*timer == 0) {
            uint8_t period = env_reg & 0x07;
            if (period > 0) {
                *timer = period;
                bool up = (env_reg & 0x08) != 0;
                if (up && *volume < 15) (*volume)++;
                else if (!up && *volume > 0) (*volume)--;
            } else {
                *timer = 8; /* If period is 0, it acts like 8 for reload? Actually spec says 8 */
            }
        }
    }
}

/* ============================================================================
 * Public Interface
 * ========================================================================== */

void* gb_audio_create(void) {
    GBAudio* apu = (GBAudio*)calloc(1, sizeof(GBAudio));
    if (!apu) return NULL;
    
    /* Approx 70224 cycles per frame / 59.7 fps = ~4.19 MHz */
    /* 4194304 / 44100 = ~95 cycles per sample */
    apu->sample_period = 95;
    
    return apu;
}

void gb_audio_destroy(void* apu) {
    free(apu);
}

void gb_audio_reset(void* apu_ptr) {
    GBAudio* apu = (GBAudio*)apu_ptr;
    memset(apu, 0, sizeof(GBAudio));
    apu->sample_period = 95;
    
    /* Initial Register Values (Standard DMG) */
    apu->ch1.nr10 = 0x80;
    apu->ch1.nr11 = 0xBF;
    apu->ch1.nr12 = 0xF3;
    apu->ch1.nr14 = 0xBF;
    
    apu->ch2.nr21 = 0x3F;
    apu->ch2.nr22 = 0x00;
    apu->ch2.nr24 = 0xBF;
    
    apu->ch3.nr30 = 0x7F;
    apu->ch3.nr31 = 0xFF;
    apu->ch3.nr32 = 0x9F;
    apu->ch3.nr34 = 0xBF;
    
    apu->ch4.nr41 = 0xFF;
    apu->ch4.nr42 = 0x00;
    apu->ch4.nr43 = 0x00;
    apu->ch4.nr44 = 0xBF;
    
    apu->nr50 = 0x77;
    apu->nr51 = 0xF3;
    apu->nr52 = 0xF1; /* Audio ON */
}

uint8_t gb_audio_read(GBContext* ctx, uint16_t addr) {
    GBAudio* apu = (GBAudio*)ctx->apu;
    if (!apu) return 0xFF;
    
    /* If audio is disabled via NR52 bit 7, most registers are 0xFF? 
       Actually, on DMG they are mostly readable. Stick to mask behavior for now. */
       
    switch (addr) {
        /* Channel 1 */
        case 0xFF10: return apu->ch1.nr10 | 0x80;
        case 0xFF11: return apu->ch1.nr11 | 0x3F;
        case 0xFF12: return apu->ch1.nr12;
        case 0xFF13: return 0xFF; /* Write only */
        case 0xFF14: return apu->ch1.nr14 | 0xBF;
        
        /* Channel 2 */
        case 0xFF15: return 0xFF; /* Not used */
        case 0xFF16: return apu->ch2.nr21 | 0x3F;
        case 0xFF17: return apu->ch2.nr22;
        case 0xFF18: return 0xFF; /* Write only */
        case 0xFF19: return apu->ch2.nr24 | 0xBF;
        
        /* Channel 3 */
        case 0xFF1A: return apu->ch3.nr30 | 0x7F;
        case 0xFF1B: return 0xFF; /* Write only */
        case 0xFF1C: return apu->ch3.nr32 | 0x9F;
        case 0xFF1D: return 0xFF; /* Write only */
        case 0xFF1E: return apu->ch3.nr34 | 0xBF;
        
        /* Channel 4 */
        case 0xFF1F: return 0xFF; /* Not used */
        case 0xFF20: return apu->ch4.nr41 | 0xFF;
        case 0xFF21: return apu->ch4.nr42;
        case 0xFF22: return apu->ch4.nr43;
        case 0xFF23: return apu->ch4.nr44 | 0xBF;
        
        /* Control */
        case 0xFF24: return apu->nr50;
        case 0xFF25: return apu->nr51;
        case 0xFF26: {
            /* NR52 - Power Status */
            /* Bit 7: Power ON/OFF (Read/Write) */
            /* Bits 0-3: Channel ON status (Read Only) */
            uint8_t val = (apu->nr52 & 0x80) | 0x70;
            if (apu->ch1.enabled) val |= 0x01;
            if (apu->ch2.enabled) val |= 0x02;
            if (apu->ch3.enabled) val |= 0x04;
            if (apu->ch4.enabled) val |= 0x08;
            return val;
        }
        
        /* Wave RAM */
        case 0xFF30: case 0xFF31: case 0xFF32: case 0xFF33:
        case 0xFF34: case 0xFF35: case 0xFF36: case 0xFF37:
        case 0xFF38: case 0xFF39: case 0xFF3A: case 0xFF3B:
        case 0xFF3C: case 0xFF3D: case 0xFF3E: case 0xFF3F:
            if (apu->ch3.enabled) {
                /* On DMG, if channel 3 is enabled, reading Wave RAM returns the byte currently being accessed */
                return apu->ch3.wave_ram[apu->ch3.wave_pos / 2];
            }
            return apu->ch3.wave_ram[addr - 0xFF30];

        default: return 0xFF;
    }
}


void gb_audio_write(GBContext* ctx, uint16_t addr, uint8_t value) {
    GBAudio* apu = (GBAudio*)ctx->apu;
    if (!apu) return;
    
    /* If APU disabled (NR52 bit 7 off), write to registers ignored unless it's NR52 or Wave RAM */
    bool power_on = (apu->nr52 & 0x80) != 0;
    
    if (addr == 0xFF26) {
        /* NR52 - Power Control */
        apu->nr52 = (apu->nr52 & 0x0F) | (value & 0x80); /* Keep status bits, update power bit */
        if (!(value & 0x80)) {
            /* Power off: clear all registers */
            memset(&apu->ch1, 0, sizeof(Channel1));
            memset(&apu->ch2, 0, sizeof(Channel2));
            memset(&apu->ch3, 0, sizeof(Channel3));
            memset(&apu->ch4, 0, sizeof(Channel4));
            apu->nr50 = 0;
            apu->nr51 = 0;
        }
        return;
    }
    
    if (!power_on && addr != 0xFF26 && !(addr >= 0xFF30 && addr <= 0xFF3F)) {
        return;
    }
    
    switch (addr) {
        /* Channel 1 */
        case 0xFF10: apu->ch1.nr10 = value; break;
        case 0xFF11: 
            apu->ch1.nr11 = value; 
            apu->ch1.length_counter = 64 - (value & 0x3F);
            break;
        case 0xFF12: 
            apu->ch1.nr12 = value; 
            /* DAC Check: If top 5 bits are 0, DAC is off and channel is disabled */
            if ((value & 0xF8) == 0) apu->ch1.enabled = false;
            break;
        case 0xFF13: apu->ch1.nr13 = value; break;
        case 0xFF14:
            apu->ch1.nr14 = value;
            apu->ch1.length_enabled = (value & 0x40) != 0;
            if (value & 0x80) {
                /* Trigger Event */
                apu->ch1.enabled = true;
                if (apu->ch1.length_counter == 0) apu->ch1.length_counter = 64;

                /* Frequency timer reload */
                apu->ch1.timer = 0;

                /* Envelope Reload */
                apu->ch1.volume = (apu->ch1.nr12 >> 4);
                apu->ch1.env_timer = (apu->ch1.nr12 & 0x07);
                if (apu->ch1.env_timer == 0) apu->ch1.env_timer = 8;

                /* Sweep Reload */
                apu->ch1.shadow_freq = apu->ch1.nr13 | ((apu->ch1.nr14 & 0x07) << 8);
                uint8_t sweep_period = (apu->ch1.nr10 >> 4) & 0x07;
                apu->ch1.sweep_timer = (sweep_period == 0) ? 8 : sweep_period;
                apu->ch1.sweep_enabled = (sweep_period > 0) || ((apu->ch1.nr10 & 0x07) > 0);
                if ((apu->ch1.nr10 & 0x07) > 0) {
                    /* Overflow check on trigger */
                    if (calc_sweep(&apu->ch1) > 2047) apu->ch1.enabled = false;
                }
            }
            break;
            
        /* Channel 2 */
        case 0xFF16: 
            apu->ch2.nr21 = value; 
            apu->ch2.length_counter = 64 - (value & 0x3F);
            break;
        case 0xFF17: 
            apu->ch2.nr22 = value; 
            if ((value & 0xF8) == 0) apu->ch2.enabled = false;
            break;
        case 0xFF18: apu->ch2.nr23 = value; break;
        case 0xFF19:
            apu->ch2.nr24 = value;
            apu->ch2.length_enabled = (value & 0x40) != 0;
            if (value & 0x80) {
                apu->ch2.enabled = true;
                if (apu->ch2.length_counter == 0) apu->ch2.length_counter = 64;

                /* Frequency timer reload */
                apu->ch2.timer = 0;

                /* Envelope Reload */
                apu->ch2.volume = (apu->ch2.nr22 >> 4);
                apu->ch2.env_timer = (apu->ch2.nr22 & 0x07);
                if (apu->ch2.env_timer == 0) apu->ch2.env_timer = 8;
            }
            break;
            
        /* Channel 3 */
        case 0xFF1A: 
            apu->ch3.nr30 = value; 
            if (!(value & 0x80)) apu->ch3.enabled = false;
            break;
        case 0xFF1B:
            apu->ch3.nr31 = value;
            apu->ch3.length_counter = 256 - value;
            break;
        case 0xFF1C: apu->ch3.nr32 = value; break;
        case 0xFF1D: apu->ch3.nr33 = value; break;
        case 0xFF1E:
            apu->ch3.nr34 = value;
            apu->ch3.length_enabled = (value & 0x40) != 0;
            if (value & 0x80) {
                apu->ch3.enabled = true;
                if (apu->ch3.length_counter == 0) apu->ch3.length_counter = 256;
                apu->ch3.wave_pos = 0;
                apu->ch3.timer = 0;
            }
            break;
            
        /* Channel 4 */
        case 0xFF20:
            apu->ch4.nr41 = value;
            apu->ch4.length_counter = 64 - (value & 0x3F);
            break;
        case 0xFF21: 
            apu->ch4.nr42 = value; 
            if ((value & 0xF8) == 0) apu->ch4.enabled = false;
            break;
        case 0xFF22: apu->ch4.nr43 = value; break;
        case 0xFF23:
            apu->ch4.nr44 = value;
            apu->ch4.length_enabled = (value & 0x40) != 0;
            if (value & 0x80) {
                apu->ch4.enabled = true;
                if (apu->ch4.length_counter == 0) apu->ch4.length_counter = 64;
                
                /* Envelope Reload */
                apu->ch4.volume = (apu->ch4.nr42 >> 4);
                apu->ch4.env_timer = (apu->ch4.nr42 & 0x07);
                if (apu->ch4.env_timer == 0) apu->ch4.env_timer = 8;
                
                /* LFSR Reload */
                apu->ch4.lfsr = 0x7FFF;
                apu->ch4.timer = 0;
            }
            break;
            
        /* Control */
        case 0xFF24: apu->nr50 = value; break;
        case 0xFF25: apu->nr51 = value; break;
        
        /* Wave RAM */
        case 0xFF30: case 0xFF31: case 0xFF32: case 0xFF33:
        case 0xFF34: case 0xFF35: case 0xFF36: case 0xFF37:
        case 0xFF38: case 0xFF39: case 0xFF3A: case 0xFF3B:
        case 0xFF3C: case 0xFF3D: case 0xFF3E: case 0xFF3F:
            /* If channel 3 is enabled, writes to Wave RAM are ignored (or weird on DMG)
             * For simplicity/safety, we block writes if enabled.
             */
            if (!apu->ch3.enabled) {
                apu->ch3.wave_ram[addr - 0xFF30] = value;
            }
            break;
    }
}

void gb_audio_step(GBContext* ctx, uint32_t cycles) {
    GBAudio* apu = (GBAudio*)ctx->apu;
    if (!apu || !(apu->nr52 & 0x80)) return;
    
    /* Advance Frame Sequencer (512 Hz) */
    /* 4194304 / 512 = 8192 cycles */
    apu->fs_timer += cycles;
    while (apu->fs_timer >= 8192) {
        apu->fs_timer -= 8192;
        apu->fs_step = (apu->fs_step + 1) & 7;
        
        /* Step Length (256 Hz): Steps 0, 2, 4, 6 */
        if ((apu->fs_step & 1) == 0) {
            /* Channel 1 Length */
            if (apu->ch1.length_enabled && apu->ch1.length_counter > 0) {
                apu->ch1.length_counter--;
                if (apu->ch1.length_counter == 0) apu->ch1.enabled = false;
            }
            /* Channel 2 Length */
            if (apu->ch2.length_enabled && apu->ch2.length_counter > 0) {
                apu->ch2.length_counter--;
                if (apu->ch2.length_counter == 0) apu->ch2.enabled = false;
            }
            /* Channel 3 Length */
            if (apu->ch3.length_enabled && apu->ch3.length_counter > 0) {
                apu->ch3.length_counter--;
                if (apu->ch3.length_counter == 0) apu->ch3.enabled = false;
            }
            /* Channel 4 Length */
            if (apu->ch4.length_enabled && apu->ch4.length_counter > 0) {
                apu->ch4.length_counter--;
                if (apu->ch4.length_counter == 0) apu->ch4.enabled = false;
            }
        }
        
        /* Step Sweep (128 Hz): Steps 2, 6 */
        if (apu->fs_step == 2 || apu->fs_step == 6) {
            Channel1* ch1 = &apu->ch1;
            if (ch1->sweep_enabled && ch1->enabled) {
                ch1->sweep_timer--;
                if (ch1->sweep_timer <= 0) {
                    /* Reload timer */
                    uint8_t period = (ch1->nr10 >> 4) & 0x07;
                    ch1->sweep_timer = (period == 0) ? 8 : period;
                    
                    if (period > 0 && (ch1->nr10 & 0x07) > 0) {
                        uint16_t new_freq = calc_sweep(ch1);
                        if (new_freq <= 2047) {
                            ch1->shadow_freq = new_freq;
                            ch1->nr13 = new_freq & 0xFF;
                            ch1->nr14 = (ch1->nr14 & ~0x07) | ((new_freq >> 8) & 0x07);
                            /* Perform semantic overflow check again */
                            if (calc_sweep(ch1) > 2047) ch1->enabled = false;
                        } else {
                            ch1->enabled = false;
                        }
                    }
                }
            }
        }
        
        /* Step Envelope (64 Hz): Step 7 */
        if (apu->fs_step == 7) {
            if (apu->ch1.enabled) step_envelope(&apu->ch1.env_timer, &apu->ch1.volume, apu->ch1.nr12);
            if (apu->ch2.enabled) step_envelope(&apu->ch2.env_timer, &apu->ch2.volume, apu->ch2.nr22);
            if (apu->ch4.enabled) step_envelope(&apu->ch4.env_timer, &apu->ch4.volume, apu->ch4.nr42);
        }
    }
    
    /* Generate Samples? */
    /* 4194304 Hz / 44100 Hz = 95.1 cycles/sample */
    apu->sample_timer += cycles;
    
    /* Channel 1 Stepping */
    if (apu->ch1.enabled) {
        uint16_t freq_raw = apu->ch1.nr13 | ((apu->ch1.nr14 & 0x07) << 8);
        uint32_t period = (2048 - freq_raw) * 4;
        if (period == 0) period = 4;
        
        apu->ch1.timer += cycles;
        while (apu->ch1.timer >= period) {
            apu->ch1.timer -= period;
            apu->ch1.wave_pos = (apu->ch1.wave_pos + 1) & 7;
        }
    }
    
    /* Channel 2 Stepping */
    if (apu->ch2.enabled) {
        uint16_t freq_raw = apu->ch2.nr23 | ((apu->ch2.nr24 & 0x07) << 8);
        uint32_t period = (2048 - freq_raw) * 4;
        if (period == 0) period = 4;
        
        apu->ch2.timer += cycles;
        while (apu->ch2.timer >= period) {
            apu->ch2.timer -= period;
            apu->ch2.wave_pos = (apu->ch2.wave_pos + 1) & 7;
        }
    }

    /* Channel 3 Stepping */
    if (apu->ch3.enabled) {
        uint16_t freq_raw = apu->ch3.nr33 | ((apu->ch3.nr34 & 0x07) << 8);
        uint32_t period = (2048 - freq_raw) * 2; /* Wave channel is 2x faster clocking? 65536Hz base */
        if (period == 0) period = 2;

        apu->ch3.timer += (cycles); /* TODO: Verify wave timing scalar */
        while (apu->ch3.timer >= period) {
            apu->ch3.timer -= period;
            apu->ch3.wave_pos = (apu->ch3.wave_pos + 1) & 31;
        }
    }

    /* Channel 4 Stepping */
    if (apu->ch4.enabled) {
        /* Polynomial Counter */
        uint8_t s = apu->ch4.nr43 >> 4;
        uint8_t r = apu->ch4.nr43 & 0x07;
        uint32_t period = NOISE_DIVISORS[r & 7] << s;
        
        /* Minimum period is 8 cycles? */
        if (period < 8) period = 8;

        /* Clock LFSR */
        apu->ch4.timer += cycles;

        while (apu->ch4.timer >= period) {
            apu->ch4.timer -= period;
            
            /* Shift LFSR */
            uint16_t lfsr = apu->ch4.lfsr;
            bool xor_res = (lfsr & 1) ^ ((lfsr >> 1) & 1);
            lfsr >>= 1;
            lfsr |= (xor_res << 14);
            if (apu->ch4.nr43 & 0x08) { /* 7-bit mode */
                lfsr &= ~0x40;
                lfsr |= (xor_res << 6);
            }
            apu->ch4.lfsr = lfsr;
        }
    }
    
    if (apu->sample_timer >= apu->sample_period) {
        apu->sample_timer -= apu->sample_period;
        
        int16_t left = 0;
        int16_t right = 0;
        
        /* Channel 1 Output */
        if (apu->ch1.enabled && (apu->ch1.nr12 & 0xF0)) { // DAC ON
             int duty = (apu->ch1.nr11 >> 6) & 3;
             int output = DUTY_CYCLES[duty][apu->ch1.wave_pos];
             
             if (output) {
                 int vol = apu->ch1.volume;
                 if (apu->nr51 & 0x01) right += vol;
                 if (apu->nr51 & 0x10) left += vol;
             }
        }
        
        /* Channel 2 Output */
        if (apu->ch2.enabled && (apu->ch2.nr22 & 0xF0)) { // DAC ON
             int duty = (apu->ch2.nr21 >> 6) & 3;
             int output = DUTY_CYCLES[duty][apu->ch2.wave_pos];
             
             if (output) {
                 int vol = apu->ch2.volume;
                 if (apu->nr51 & 0x02) right += vol;
                 if (apu->nr51 & 0x20) left += vol;
             }
        }

        /* Channel 3 Output */
        if (apu->ch3.enabled && (apu->ch3.nr30 & 0x80)) { // DAC ON
             /* Get wave sample (4-bit) */
             uint8_t byte = apu->ch3.wave_ram[apu->ch3.wave_pos / 2];
             uint8_t sample = (apu->ch3.wave_pos & 1) ? (byte & 0x0F) : (byte >> 4);
             
             /* Apply volume shift */
             uint8_t vol_code = (apu->ch3.nr32 >> 5) & 3;
             switch (vol_code) {
                 case 0: sample = 0; break; /* Mute */
                 case 1: break; /* 100% */
                 case 2: sample >>= 1; break; /* 50% */
                 case 3: sample >>= 2; break; /* 25% */
             }
             
             if (apu->nr51 & 0x04) right += sample;
             if (apu->nr51 & 0x40) left += sample;
        }

        /* Channel 4 Output */
        if (apu->ch4.enabled && (apu->ch4.nr42 & 0xF0)) { // DAC ON
             bool output = !(apu->ch4.lfsr & 1); /* Output is inverted bit 0 */
             
             if (output) {
                 int vol = apu->ch4.volume;
                 if (apu->nr51 & 0x08) right += vol;
                 if (apu->nr51 & 0x80) left += vol;
             }
        }
        
        /* Master Volume / Scaling */
        /* Currently values are 0-15 per channel, mixed. Max approx 60. */
        /* Scale to int16 range */
        /* Max possible value: 15 * 4 * 8 = 480. 
           32767 / 480 = 68. 
           Using 64 provides good volume without clipping. */
        int vol_l = (apu->nr50 >> 4) & 7;
        int vol_r = (apu->nr50 & 7);
        
        left = left * (vol_l + 1) * 64;
        right = right * (vol_r + 1) * 64;

        gb_audio_callback(ctx, left, right);
    }
}

void gb_audio_div_reset(void* apu_ptr) {
    if (!apu_ptr) return;
    GBAudio* apu = (GBAudio*)apu_ptr;
    
    /* When DIV is reset, the internal counter for the Frame Sequencer is NOT affected 
       directly, but the way Game Boy hardware derives the 512 Hz clock is from 
       specific bits of the DIV counter. 
       
       However, a simplified behavior that is commonly used is that resetting DIV
       effectively aligns the Frame Sequencer timer.
       
       Exact behavior:
       Frame Sequencer clocked by Bit 12 (falling edge presumably) of the internal DIV counter.
       Writing to DIV resets internal counter to 0. 
       So we should reset our accumulation timer.
    */
    apu->fs_timer = 0;
}
