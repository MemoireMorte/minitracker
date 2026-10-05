// Minitracker
//
// Sequenceur pas a pas pour la carte Minitel Wifi V2 (ESP32-WROOM-32) :
// grille de 16 pas, 4 pistes melodiques et 2 pistes de batterie, editee au
// clavier du Minitel, lecture en boucle sur une enceinte Bluetooth (A2DP)
// appairee depuis le Minitel.
// Mode d'emploi et notes de conception : README.md
//
// Bibliotheques : ESP32-A2DP, Minitel1B_Hard
// Carte : ESP32 Dev Module - Partition : Huge APP (3MB No OTA/1MB SPIFFS)

#include <Preferences.h>
#include <Minitel1B_Hard.h>
#include "BluetoothA2DPSource.h"

// Types definis plus bas ; declares ici pour les prototypes generes par l'IDE.
struct Boucle;
struct Tonal;

// ---- Reglages --------------------------------------------------------------
const uint8_t VOLUME_BT = 60;      // 0 a 127
const int16_t AMPLITUDE = 6000;    // niveau maximal d'une voix a volume 1.0
volatile uint16_t tempoBpm = 120;
// Volume de chaque piste, de 0 (muette) a 9. Reglable au clavier (+ / -).
// La basse (P4) est renforcee par defaut.
uint8_t niveauPiste[4] = { 5, 4, 4, 8 };
volatile float volumePiste[4];
uint8_t niveauBatterie[2] = { 6, 6 };  // volume des 2 pistes de batterie, 0 a 9
volatile float volumeBatterie[2];
void appliquerVolumes() {
  for (int p = 0; p < 4; p++) volumePiste[p] = niveauPiste[p] / 6.0f;
  for (int b = 0; b < 2; b++) volumeBatterie[b] = niveauBatterie[b] / 6.0f;
}

// Forme d'onde de chaque piste. Reglable au clavier (touche *).
enum : uint8_t { ONDE_CARREE, ONDE_SCIE, ONDE_TRIANGLE, ONDE_SINUS, NB_ONDES };
const char* const NOM_ONDE[NB_ONDES] = { "CAR", "SCI", "TRI", "SIN" };
volatile uint8_t formePiste[4] = { ONDE_CARREE, ONDE_CARREE, ONDE_CARREE, ONDE_CARREE };

// Mettre a 1 pour afficher le code de chaque touche sur le moniteur serie.
#define DEBUG_TOUCHES 0
// ----------------------------------------------------------------------------

const int   LED_PIN        = 13;
const float FREQ_ECHANT_HZ = 44100.0f;
const int   NB_PAS         = 16;
const int   NB_PISTES      = 4;              // pistes melodiques
const int   NB_BATTERIES   = 2;              // pistes de batterie
const int   NB_COLONNES    = NB_PISTES + NB_BATTERIES;  // colonnes de la grille
const int   COL_BATTERIE   = NB_PISTES;      // premiere colonne de batterie

Minitel             minitel(Serial2, 16, 17);
BluetoothA2DPSource a2dp;
Preferences         prefs;

// ============================================================================
// Moteur sonore (execute dans la tache Bluetooth)
// ============================================================================

// Notes en numeros MIDI (nC4 = do central = 60). 0 = case vide.
// Prefixe "n" : A3, A4... sont deja des noms de broches dans le coeur ESP32.
enum : uint8_t {
  ___ = 0,
  nC2 = 36, nD2 = 38, nE2 = 40, nF2 = 41, nG2 = 43, nA2 = 45, nB2 = 47,
  nC3 = 48, nD3 = 50, nE3 = 52, nF3 = 53, nG3 = 55, nA3 = 57, nB3 = 59,
  nC4 = 60, nD4 = 62, nE4 = 64, nF4 = 65, nG4 = 67, nA4 = 69, nB4 = 71,
  nC5 = 72, nD5 = 74, nE5 = 76, nF5 = 77, nG5 = 79, nA5 = 81, nB5 = 83,
};

volatile uint8_t grille[NB_PAS][NB_PISTES] = {
  { nA4, nE4, nC4, nA2 }, { ___, ___, ___, ___ }, { nC5, ___, ___, nA2 }, { nE5, ___, ___, ___ },
  { nF4, nC4, nA3, nF2 }, { ___, ___, ___, ___ }, { nA4, ___, ___, nF2 }, { nC5, ___, ___, ___ },
  { nE4, nC4, nG3, nC3 }, { ___, ___, ___, ___ }, { nG4, ___, ___, nC3 }, { nC5, ___, ___, ___ },
  { nD4, nB3, nG3, nG2 }, { ___, ___, ___, ___ }, { nG4, ___, ___, nG2 }, { nB4, ___, ___, nD3 },
};

volatile bool    lecture    = false;
volatile uint8_t pasCourant = 0;

struct Voix {
  uint32_t phase;
  uint32_t increment;
  float    niveau;
  float    cible;
};

Voix     voix[NB_PISTES];
uint32_t incrementNote[128];
float    tableSinus[1024];

// Valeur de l'onde (-1 a +1) pour une phase sur 32 bits. Le triangle et le
// sinus, pauvres en harmoniques, sont un peu amplifies pour paraitre aussi
// forts que le carre.
inline float echantillonOnde(uint8_t forme, uint32_t phase) {
  switch (forme) {
    case ONDE_SCIE:
      return (int32_t)phase * (1.0f / 2147483648.0f);
    case ONDE_TRIANGLE: {
      float t = phase * (1.0f / 4294967296.0f);            // 0 a 1
      return (4.0f * fabsf(t - 0.5f) - 1.0f) * 1.4f;
    }
    case ONDE_SINUS:
      return tableSinus[phase >> 22] * 1.4f;
    default:  // ONDE_CARREE
      return (phase & 0x80000000u) ? 1.0f : -1.0f;
  }
}

const float PAS_ATTAQUE   = 1.0f / (0.003f * FREQ_ECHANT_HZ);
const float COEF_DECLIN   = 0.99988f;
const float SEUIL_SILENCE = 0.0005f;

uint32_t echantillonsRestants = 0;
uint8_t  pasSuivant           = 0;

inline uint32_t echantillonsParPas() {
  return (uint32_t)(FREQ_ECHANT_HZ * 60.0f / (tempoBpm * 4.0f));
}

// ---- Batterie synthetique ---------------------------------------------------
//
// Deux pistes, un son par pas et par piste. Les sons sont calcules, sans
// echantillons : sinus a frequence glissante pour les kicks et le tom, bruit
// blanc pour la caisse claire, bruit filtre passe-haut pour les hi-hats.
// Les deux pistes declenchent les memes voix (une par son) : elles servent a
// superposer deux sons differents sur un meme pas. Chaque voix retient le
// volume de la piste qui l'a declenchee.

// Les valeurs sont enregistrees dans les boucles : ajouter a la fin seulement.
enum : uint8_t { BAT_VIDE, BAT_KICK, BAT_SNARE, BAT_HAT, BAT_OPEN, BAT_TOM, BAT_GABBER, NB_BAT };
const char* const NOM_BAT[NB_BAT] = { "---", "KIK", "SNR", "CHH", "OHH", "TOM", "GAB" };

// Motif de batterie accompagnant le motif de test.
volatile uint8_t batterie[NB_BATTERIES][NB_PAS] = {
  { BAT_KICK,  BAT_VIDE, BAT_VIDE, BAT_VIDE,      // D1 : kicks et caisse claire
    BAT_SNARE, BAT_VIDE, BAT_VIDE, BAT_VIDE,
    BAT_KICK,  BAT_VIDE, BAT_VIDE, BAT_KICK,
    BAT_SNARE, BAT_VIDE, BAT_VIDE, BAT_VIDE },
  { BAT_HAT,   BAT_VIDE, BAT_HAT,  BAT_VIDE,      // D2 : hi-hats
    BAT_HAT,   BAT_VIDE, BAT_HAT,  BAT_VIDE,
    BAT_HAT,   BAT_VIDE, BAT_HAT,  BAT_VIDE,
    BAT_HAT,   BAT_VIDE, BAT_OPEN, BAT_VIDE },
};

// Son "tonal" : un sinus dont la frequence glisse de freq vers freqFin.
struct Tonal {
  uint32_t phase;
  float    freq, freqFin, glisse;   // glisse : rapprochement par echantillon
  float    env, declin;             // enveloppe et son coefficient de declin
  float    gain;                    // volume de la piste qui a declenche le son
};
Tonal kick   = { 0, 0, 0, 0, 0, 0, 0 };
Tonal tom    = { 0, 0, 0, 0, 0, 0, 0 };
Tonal gabber = { 0, 0, 0, 0, 0, 0, 0 };

// Kick gabber : sinus glissant de tres haut, tenu longtemps, puis ecrase par
// une forte saturation qui le rapproche d'une onde carree.
const float GABBER_SATURATION = 9.0f;    // plus c'est grand, plus c'est sale
const float GABBER_NIVEAU     = 0.9f;    // niveau de sortie, les autres sons sont vers 1

// Niveau general de la batterie par rapport aux pistes melodiques.
const float NIVEAU_BATTERIE   = 3.0f;

float    envBruitCaisse = 0.0f, envTonCaisse = 0.0f, gainCaisse = 0.0f;
uint32_t phaseCaisse    = 0;
float    envCharley     = 0.0f, declinCharley = 0.9985f, bruitPrecedent = 0.0f;
float    gainCharley    = 0.0f;
uint32_t graineBruit    = 0x2545F491u;

inline float bruitBlanc() {              // generateur xorshift, -1 a +1
  graineBruit ^= graineBruit << 13;
  graineBruit ^= graineBruit >> 17;
  graineBruit ^= graineBruit << 5;
  return (int32_t)graineBruit * (1.0f / 2147483648.0f);
}

// gain : volume de la piste qui declenche le son.
void declencherBatterie(uint8_t son, float gain) {
  switch (son) {
    case BAT_KICK:   kick   = { 0, 160.0f,  52.0f, 0.9988f,  1.0f, 0.99977f, gain }; break;
    case BAT_TOM:    tom    = { 0, 220.0f, 110.0f, 0.9993f,  1.0f, 0.99975f, gain }; break;
    case BAT_GABBER: gabber = { 0, 420.0f,  58.0f, 0.99915f, 1.0f, 0.99986f, gain }; break;
    case BAT_SNARE:  envBruitCaisse = 1.0f; envTonCaisse = 1.0f; phaseCaisse = 0;
                     gainCaisse = gain; break;
    case BAT_HAT:    envCharley = 1.0f; declinCharley = 0.9985f;  gainCharley = gain; break;  // ferme : ~15 ms
    case BAT_OPEN:   envCharley = 1.0f; declinCharley = 0.99981f; gainCharley = gain; break;  // ouvert : ~120 ms
  }
}

inline float echantillonTonal(Tonal& t) {
  if (t.env < SEUIL_SILENCE) return 0.0f;
  t.freq = t.freqFin + (t.freq - t.freqFin) * t.glisse;
  t.phase += (uint32_t)(t.freq * (4294967296.0f / FREQ_ECHANT_HZ));
  float s = tableSinus[t.phase >> 22] * t.env;
  t.env *= t.declin;
  return s;
}

inline float echantillonBatterie() {
  float s = echantillonTonal(kick) * 1.6f * kick.gain
          + echantillonTonal(tom)  * 1.2f * tom.gain;

  if (gabber.env >= SEUIL_SILENCE) {               // kick gabber : sinus sature
    float g = echantillonTonal(gabber) * GABBER_SATURATION;
    g = g / (1.0f + fabsf(g));                     // ecretage doux, reste entre -1 et +1
    s += g * GABBER_NIVEAU * gabber.gain;
  }

  if (envBruitCaisse > SEUIL_SILENCE) {            // caisse claire : bruit + un peu de ton
    phaseCaisse += (uint32_t)(190.0f * (4294967296.0f / FREQ_ECHANT_HZ));
    s += (bruitBlanc() * envBruitCaisse * 0.8f
          + tableSinus[phaseCaisse >> 22] * envTonCaisse * 0.6f) * gainCaisse;
    envBruitCaisse *= 0.99962f;
    envTonCaisse   *= 0.99943f;
  }
  if (envCharley > SEUIL_SILENCE) {                // charley : bruit passe-haut
    float b = bruitBlanc();
    s += (b - bruitPrecedent) * 0.35f * envCharley * gainCharley;
    bruitPrecedent = b;
    envCharley *= declinCharley;
  }
  return s;
}

inline void declencherPas(uint8_t pas) {
  for (int p = 0; p < NB_PISTES; p++) {
    uint8_t note = grille[pas][p];
    if (note == 0) continue;
    voix[p].increment = incrementNote[note & 0x7F];
    voix[p].cible = 1.0f;
  }
  for (int b = 0; b < NB_BATTERIES; b++)
    if (batterie[b][pas]) declencherBatterie(batterie[b][pas], volumeBatterie[b]);
  pasCourant = pas;
}

// ---- Boucles : la boucle de travail et les 9 emplacements ------------------
//
// La boucle de travail (grille, tempo, volumes, ondes) ne vit qu'en memoire
// vive. Elle n'est ecrite en memoire flash que sur demande, dans l'un des
// 9 emplacements. Eteindre sans sauver perd les modifications.

struct Boucle {
  uint8_t  grille[NB_PAS * NB_PISTES];
  uint16_t tempo;
  uint8_t  volume[4];
  uint8_t  onde[4];
  // Ajoutes ensuite, toujours a la fin : une boucle plus ancienne n'a que le
  // debut de la structure et se relit avec les pistes manquantes vides.
  uint8_t  batterie[NB_PAS];        // piste D1 (format 2)
  uint8_t  volumeBatterie;
  uint8_t  batterie2[NB_PAS];       // piste D2 (format 3)
  uint8_t  volumeBatterie2;
};
const size_t TAILLE_BOUCLE_V1 = 74;   // sans batterie
const size_t TAILLE_BOUCLE_V2 = 92;   // une piste de batterie

int octaveCourante = 4;                    // octave donnee aux nouvelles notes

const int NB_EMPLACEMENTS = 9;
Boucle emplacements[NB_EMPLACEMENTS];      // copie en memoire vive de la flash
bool   occupe[NB_EMPLACEMENTS];

int  emplacementCourant = 0;               // 1 a 9 ; 0 = boucle jamais sauvee
bool modifie            = false;           // differe de l'emplacement courant

// Chargement differe : pendant la lecture, la nouvelle boucle prend effet
// au debut du tour suivant, dans le rappel audio.
Boucle        boucleEnAttente;
int           emplacementDemande = 0;
volatile bool chargementDemande  = false;
volatile bool chargementFait     = false;  // signale a la boucle principale

void capturerBoucle(Boucle& b) {
  memcpy(b.grille, (const void*)grille, sizeof(b.grille));
  b.tempo = tempoBpm;
  for (int p = 0; p < 4; p++) { b.volume[p] = niveauPiste[p]; b.onde[p] = formePiste[p]; }
  memcpy(b.batterie,  (const void*)batterie[0], sizeof(b.batterie));
  memcpy(b.batterie2, (const void*)batterie[1], sizeof(b.batterie2));
  b.volumeBatterie  = niveauBatterie[0];
  b.volumeBatterie2 = niveauBatterie[1];
}

void appliquerBoucle(const Boucle& b) {
  memcpy((void*)grille, b.grille, sizeof(b.grille));
  tempoBpm = (b.tempo >= 60 && b.tempo <= 200) ? b.tempo : 120;
  for (int p = 0; p < 4; p++) {
    niveauPiste[p] = b.volume[p] <= 9 ? b.volume[p] : 9;
    formePiste[p]  = b.onde[p] < NB_ONDES ? b.onde[p] : ONDE_CARREE;
  }
  for (int pas = 0; pas < NB_PAS; pas++)
  {
    batterie[0][pas] = b.batterie[pas]  < NB_BAT ? b.batterie[pas]  : BAT_VIDE;
    batterie[1][pas] = b.batterie2[pas] < NB_BAT ? b.batterie2[pas] : BAT_VIDE;
  }
  niveauBatterie[0] = b.volumeBatterie  <= 9 ? b.volumeBatterie  : 9;
  niveauBatterie[1] = b.volumeBatterie2 <= 9 ? b.volumeBatterie2 : 9;
  appliquerVolumes();
}

int32_t produireAudio(Frame* trames, int32_t nb) {
  for (int32_t i = 0; i < nb; i++) {
    if (lecture) {
      if (echantillonsRestants == 0) {
        if (pasSuivant == 0 && chargementDemande) {   // debut de tour
          appliquerBoucle(boucleEnAttente);
          chargementDemande = false;
          chargementFait = true;
        }
        declencherPas(pasSuivant);
        pasSuivant = (pasSuivant + 1) % NB_PAS;
        echantillonsRestants = echantillonsParPas();
      }
      echantillonsRestants--;
    }

    float somme = 0.0f;
    for (int p = 0; p < NB_PISTES; p++) {
      Voix& v = voix[p];
      if (v.cible > 0.0f) {
        v.niveau += PAS_ATTAQUE;
        if (v.niveau >= 1.0f) { v.niveau = 1.0f; v.cible = 0.0f; }
      } else if (v.niveau > SEUIL_SILENCE) {
        v.niveau *= COEF_DECLIN;
      } else {
        v.niveau = 0.0f;
        continue;
      }
      v.phase += v.increment;
      somme += echantillonOnde(formePiste[p], v.phase) * v.niveau * volumePiste[p];
    }

    somme += echantillonBatterie() * NIVEAU_BATTERIE;   // volumes de piste deja appliques

    float s = somme * AMPLITUDE;
    if (s > 32767.0f)  s = 32767.0f;      // ecretage de securite
    if (s < -32768.0f) s = -32768.0f;
    trames[i].channel1 = (int16_t)s;
    trames[i].channel2 = (int16_t)s;
  }
  return nb;
}

// ============================================================================
// Bluetooth : liste des appareils trouves et choix de l'enceinte
// ============================================================================

const int MAX_APPAREILS = 9;
const int LONGUEUR_NOM  = 24;

struct Appareil {
  uint8_t adresse[6];
  char    nom[LONGUEUR_NOM + 1];
  int     rssi;
};

Appareil     appareils[MAX_APPAREILS];
volatile int nbAppareils      = 0;
volatile int versionListe     = 0;      // incrementee a chaque ajout
portMUX_TYPE verrouListe      = portMUX_INITIALIZER_UNLOCKED;

uint8_t       adresseChoisie[6];
volatile bool choixFait       = false;  // une enceinte est designee
char          nomChoisi[LONGUEUR_NOM + 1] = "";

// Copie un nom Bluetooth en ne gardant que l'ASCII affichable par le Minitel.
void nettoyerNom(char* dest, const char* src) {
  int n = 0;
  for (; src && src[n] && n < LONGUEUR_NOM; n++) {
    uint8_t c = (uint8_t)src[n];
    dest[n] = (c >= 32 && c <= 126) ? (char)c : '?';
  }
  dest[n] = 0;
}

// Appelee par la pile Bluetooth pour chaque appareil audio trouve.
// Renvoyer true = se connecter a cet appareil.
bool appareilTrouve(const char* nom, esp_bd_addr_t adresse, int rssi) {
  portENTER_CRITICAL(&verrouListe);
  bool connu = false;
  for (int i = 0; i < nbAppareils; i++) {
    if (memcmp(appareils[i].adresse, adresse, 6) == 0) { connu = true; break; }
  }
  if (!connu && nbAppareils < MAX_APPAREILS) {
    Appareil& a = appareils[nbAppareils];
    memcpy(a.adresse, adresse, 6);
    nettoyerNom(a.nom, nom);
    a.rssi = rssi;
    nbAppareils = nbAppareils + 1;
    versionListe = versionListe + 1;
  }
  bool accepter = choixFait && memcmp(adresseChoisie, adresse, 6) == 0;
  portEXIT_CRITICAL(&verrouListe);
  return accepter;
}

// ============================================================================
// Ecrans Minitel
// ============================================================================

// Retard de l'affichage du repere de lecture, pour compenser la latence de
// l'enceinte Bluetooth. A ajuster a l'oreille (0 a 500 ms).
const uint16_t RETARD_AFFICHAGE_MS = 150;

const int Y_GRILLE = 5;                       // ligne ecran du pas 1
inline int xPiste(int p) { return 8 + 6 * p; }  // colonne ecran d'une piste

int vitesseMinitel = 1200;   // bauds, mesuree au demarrage

// Positionnement court : 3 octets (US, ligne, colonne) au lieu des 6 a 8
// octets de minitel.moveCursorXY(), qui passe par une sequence ESC [ y ; x H.
// Remet aussi les attributs d'affichage a leur valeur par defaut.
inline void aller(int x, int y) {
  minitel.writeByte(0x1F);
  minitel.writeByte(0x40 + y);
  minitel.writeByte(0x40 + x);
}

// Ecrit une ligne complete (40 colonnes), completee par des espaces.
void ligne(int y, String texte) {
  while (texte.length() < 39) texte += ' ';
  aller(1, y);
  minitel.print(texte.substring(0, 39));
}

void bandeau(String texte) {
  aller(1, 1);
  minitel.attributs(INVERSION_FOND);
  String t = " " + texte;
  while (t.length() < 39) t += ' ';
  minitel.print(t.substring(0, 39));
  minitel.attributs(FOND_NORMAL);
}

void titre(const char* texte) {
  minitel.newScreen();
  minitel.noCursor();
  bandeau(texte);
}

const char* etoiles(int rssi) {
  if (rssi > -55) return "****";
  if (rssi > -65) return "***";
  if (rssi > -75) return "**";
  return "*";
}

// ---- Ecran de recherche ----------------------------------------------------

void ecranRecherche() {
  titre("ENCEINTE BLUETOOTH");
  if (choixFait) {
    ligne(3, String("Connexion a ") + nomChoisi + "...");
  } else {
    ligne(3, "Recherche en cours...");
  }
  ligne(4, "Mettez l'enceinte en mode appairage.");
  ligne(21, "1-9 : choisir une enceinte");
  ligne(22, "REPETITION : relancer la recherche");
}

void afficherListe() {
  Appareil copie[MAX_APPAREILS];
  portENTER_CRITICAL(&verrouListe);
  int n = nbAppareils;
  memcpy(copie, appareils, sizeof(copie));
  portEXIT_CRITICAL(&verrouListe);

  for (int i = 0; i < n; i++) {
    char buf[41];
    snprintf(buf, sizeof(buf), " %d  %-24s %02X%02X %s", i + 1,
             copie[i].nom[0] ? copie[i].nom : "(sans nom)",
             copie[i].adresse[4], copie[i].adresse[5], etoiles(copie[i].rssi));
    ligne(6 + i, buf);
  }
}

// ---- Ecran sommaire : boucles et enceinte ----------------------------------

// Frise de 16 caracteres : '#' si le pas contient au moins une note.
void friseBoucle(const Boucle& b, char* dest) {
  for (int pas = 0; pas < NB_PAS; pas++) {
    bool note = false;
    for (int p = 0; p < NB_PISTES; p++) if (b.grille[pas * NB_PISTES + p]) note = true;
    if (b.batterie[pas] || b.batterie2[pas]) note = true;
    dest[pas] = note ? '#' : '.';
  }
  dest[NB_PAS] = 0;
}

void afficherEmplacement(int n) {            // n = 1 a 9
  char buf[41];
  if (occupe[n - 1]) {
    char frise[NB_PAS + 1];
    friseBoucle(emplacements[n - 1], frise);
    snprintf(buf, sizeof(buf), " %d  %3u BPM  %s%s", n, emplacements[n - 1].tempo,
             frise, n == emplacementCourant ? "  <" : "");
  } else {
    snprintf(buf, sizeof(buf), " %d  (vide)", n);
  }
  ligne(4 + n, buf);
}

void afficherEtatSommaire() {
  char buf[41];
  if (emplacementCourant) {
    snprintf(buf, sizeof(buf), "Boucle en cours : %d%s", emplacementCourant,
             modifie ? " (modifiee, non sauvee)" : "");
  } else {
    snprintf(buf, sizeof(buf), "Boucle en cours : jamais sauvee");
  }
  ligne(3, buf);
}

void messageSommaire(const char* texte) { ligne(15, texte); }

void ecranSommaire() {
  titre("SOMMAIRE");
  afficherEtatSommaire();
  for (int n = 1; n <= NB_EMPLACEMENTS; n++) afficherEmplacement(n);
  ligne(17, "1-9 : charger une boucle");
  ligne(18, "S puis 1-9 : sauver la boucle en cours");
  ligne(20, String("Enceinte : ") + nomChoisi);
  ligne(21, "ANNULATION : changer d'enceinte");
  ligne(23, "SOMMAIRE : retour a la grille");
}

// ---- Ecran de la grille ----------------------------------------------------

// Convertit une note MIDI en texte sur 3 caracteres : "C4 ", "F#3", "---".
void texteNote(uint8_t note, char* dest) {
  static const char* NOMS[12] = { "C", "C#", "D", "D#", "E", "F",
                                  "F#", "G", "G#", "A", "A#", "B" };
  if (note == 0) { strcpy(dest, "---"); return; }
  snprintf(dest, 4, "%s%d", NOMS[note % 12], note / 12 - 1);
  while (strlen(dest) < 3) strcat(dest, " ");
}

void afficherBandeauGrille() {
  char buf[41];
  // "BOUCLE 3*" : emplacement courant, etoile si modifiee depuis la sauvegarde
  snprintf(buf, sizeof(buf), "BOUCLE %c%c  %3u BPM  OCT %d  %s",
           emplacementCourant ? '0' + emplacementCourant : '-',
           modifie ? '*' : ' ',
           tempoBpm, octaveCourante, lecture ? "LECTURE" : "ARRET  ");
  bandeau(buf);
}

// Signale que la boucle de travail a change depuis la derniere sauvegarde.
// Appelee uniquement depuis l'ecran de la grille.
void marquerModifie() {
  if (modifie) return;
  modifie = true;
  afficherBandeauGrille();     // fait apparaitre l'etoile
}

int curPas = 0, curPiste = 0;   // case sous le curseur

// Dessine une case ; celle du curseur est en video inverse.
void afficherCase(int pas, int piste) {
  char t[4];
  if (piste >= COL_BATTERIE) {
    uint8_t son = batterie[piste - COL_BATTERIE][pas];
    strcpy(t, NOM_BAT[son < NB_BAT ? son : 0]);
  } else {
    texteNote(grille[pas][piste], t);
  }
  bool curseur = (pas == curPas && piste == curPiste);
  aller(xPiste(piste), Y_GRILLE + pas);
  if (curseur) minitel.attributs(INVERSION_FOND);
  minitel.print(t);
  if (curseur) minitel.attributs(FOND_NORMAL);
}

void afficherLigneEtat() {
  char buf[41];
  snprintf(buf, sizeof(buf), "  ONDE %s   %s   %s   %s",
           NOM_ONDE[formePiste[0]], NOM_ONDE[formePiste[1]],
           NOM_ONDE[formePiste[2]], NOM_ONDE[formePiste[3]]);
  ligne(2, buf);
}

void afficherVolumes() {
  char buf[41];
  snprintf(buf, sizeof(buf), "  VOL  %d     %d     %d     %d     %d     %d",
           niveauPiste[0], niveauPiste[1], niveauPiste[2], niveauPiste[3],
           niveauBatterie[0], niveauBatterie[1]);
  ligne(3, buf);
}

void afficherLignePas(int pas) {
  int y = Y_GRILLE + pas;
  char num[3];
  snprintf(num, sizeof(num), "%02d", pas + 1);

  aller(3, y);
  if (pas % 4 == 0) minitel.attributs(INVERSION_FOND);   // repere des temps
  minitel.print(num);
  if (pas % 4 == 0) minitel.attributs(FOND_NORMAL);

  for (int p = 0; p < NB_COLONNES; p++) afficherCase(pas, p);
}

int reperePose = -1;   // pas ou le repere ">" est actuellement dessine

// Nombre de pas entre deux deplacements du repere (1, 2 ou 4), choisi pour
// que l'affichage n'occupe pas plus de 60 % du debit de la liaison.
// Un deplacement coute 8 octets (effacer l'ancien repere, dessiner le nouveau).
int pasDuRepere() {
  float pasParSeconde = tempoBpm * 4.0f / 60.0f;
  float budget = 0.6f * vitesseMinitel / 10.0f;      // octets par seconde
  float besoin = pasParSeconde * 8.0f;
  if (besoin <= budget)        return 1;
  if (besoin <= budget * 2.0f) return 2;
  return 4;
}

void poserRepere(int pas) {
  if (pas == reperePose) return;
  if (reperePose >= 0) {
    aller(1, Y_GRILLE + reperePose);
    minitel.print(" ");
  }
  if (pas >= 0) {
    aller(1, Y_GRILLE + pas);
    minitel.print(">");
  }
  reperePose = pas;
}

void ecranGrille() {
  minitel.newScreen();
  minitel.noCursor();
  afficherBandeauGrille();
  afficherLigneEtat();
  afficherVolumes();
  ligne(4, "  PAS  P1    P2    P3    P4    D1    D2");
  for (int pas = 0; pas < NB_PAS; pas++) afficherLignePas(pas);
  ligne(22, "A-G # 1-7 notes   D1 D2: K G N C O T");
  ligne(23, "+/- volume  * onde  ESPACE vider");
  ligne(24, "ENVOI lecture  S sauver  GUIDE aide");
  reperePose = -1;
}

// ---- Ecran d'aide ----------------------------------------------------------

void ecranAide() {
  titre("AIDE");
  ligne(3,  "Fleches     deplacer le curseur");
  ligne(4,  "A a G       poser une note");
  ligne(5,  "#           diese (sauf E et B)");
  ligne(6,  "1 a 7       octave");
  ligne(7,  "ESPACE      vider la case");
  ligne(8,  "CORRECTION  vider la case");
  ligne(9,  "+ et -      volume de la piste (0-9)");
  ligne(10, "*           forme d'onde de la piste");
  ligne(11, "ENVOI       lecture / arret");
  ligne(12, "SUITE       tempo + 5 BPM");
  ligne(13, "RETOUR      tempo - 5 BPM");
  ligne(14, "S           sauver la boucle");
  ligne(15, "ANNULATION  deux fois : tout vider");
  ligne(16, "SOMMAIRE    boucles et enceinte");
  ligne(17, "Pistes D1 D2 : K kick  G kick gabber");
  ligne(18, " N snare  T tom  C hi-hat ferme (CHH)");
  ligne(19, " O hi-hat ouvert (OHH)");
  ligne(20, "Ondes : CARree, dent de SCIe,");
  ligne(21, "TRIangle, SINus.");
  ligne(22, "Rien n'est sauve sans la touche S.");
  ligne(24, "Une touche pour revenir a la grille");
}

// ---- Edition de la grille --------------------------------------------------

// Joue une note une fois sur une piste (ecoute a l'arret).
void ecouter(int piste, uint8_t note) {
  voix[piste].increment = incrementNote[note & 0x7F];
  voix[piste].cible = 1.0f;
}

bool estDiese(uint8_t note) {
  int d = note % 12;
  return d == 1 || d == 3 || d == 6 || d == 8 || d == 10;
}

// Demi-ton correspondant a une lettre de note, ou -1.
int demiTonLettre(unsigned long touche) {
  switch (touche) {
    case 'C': case 'c': return 0;
    case 'D': case 'd': return 2;
    case 'E': case 'e': return 4;
    case 'F': case 'f': return 5;
    case 'G': case 'g': return 7;
    case 'A': case 'a': return 9;
    case 'B': case 'b': return 11;
  }
  return -1;
}

bool estFleche(unsigned long touche) {
  return touche == TOUCHE_FLECHE_HAUT || touche == TOUCHE_FLECHE_BAS ||
         touche == TOUCHE_FLECHE_GAUCHE || touche == TOUCHE_FLECHE_DROITE ||
         (touche >= 0x08 && touche <= 0x0B);
}

// Saisie sur une piste de batterie : une lettre par son.
void toucheBatterie(unsigned long touche) {
  int b = curPiste - COL_BATTERIE;         // 0 = D1, 1 = D2
  uint8_t ancien = batterie[b][curPas];
  uint8_t nouveau;
  switch (touche) {
    case 'k': case 'K': nouveau = BAT_KICK;  break;
    case 'g': case 'G': nouveau = BAT_GABBER; break;
    case 'n': case 'N': nouveau = BAT_SNARE; break;
    case 'c': case 'C': nouveau = BAT_HAT;   break;   // closed hi-hat
    case 'o': case 'O': nouveau = BAT_OPEN;  break;   // open hi-hat
    case 't': case 'T': nouveau = BAT_TOM;   break;
    case ' ': case CORRECTION: nouveau = BAT_VIDE; break;
    default: return;                                     // touche sans effet
  }
  if (nouveau != ancien) {
    batterie[b][curPas] = nouveau;
    afficherCase(curPas, curPiste);
    marquerModifie();
  }
  if (nouveau != BAT_VIDE && !lecture) declencherBatterie(nouveau, volumeBatterie[b]);   // ecoute
}

// Traite une touche d'edition sur l'ecran de la grille.
void toucheEdition(unsigned long touche) {
  bool surBatterie = (curPiste >= COL_BATTERIE);

  // Volume de la piste sous le curseur
  if (touche == '+' || touche == '-') {
    uint8_t& n = surBatterie ? niveauBatterie[curPiste - COL_BATTERIE] : niveauPiste[curPiste];
    if (touche == '+' && n < 9)      n++;
    else if (touche == '-' && n > 0) n--;
    else { minitel.bip(); return; }
    appliquerVolumes();
    afficherVolumes();
    marquerModifie();
    return;
  }

  // Forme d'onde de la piste sous le curseur
  if (touche == '*') {
    if (surBatterie) { minitel.bip(); return; }          // pas de forme d'onde
    formePiste[curPiste] = (formePiste[curPiste] + 1) % NB_ONDES;
    afficherLigneEtat();
    marquerModifie();
    uint8_t n = grille[curPas][curPiste];
    if (n != 0 && !lecture) ecouter(curPiste, n);   // apercu du nouveau timbre
    return;
  }

  if (surBatterie && !estFleche(touche)) { toucheBatterie(touche); return; }

  int ancienPas = curPas, anciennePiste = curPiste;
  uint8_t note = surBatterie ? 0 : grille[curPas][curPiste];
  int nouvelle = note;          // -1 = pas de changement de note
  bool noteSaisie = false;

  // Fleches : sequences du clavier etendu, ou codes simples (0x08 a 0x0B)
  // selon le mode du clavier.
  if      (touche == TOUCHE_FLECHE_HAUT   || touche == 0x0B) curPas   = (curPas + NB_PAS - 1) % NB_PAS;
  else if (touche == TOUCHE_FLECHE_BAS    || touche == 0x0A) curPas   = (curPas + 1) % NB_PAS;
  else if (touche == TOUCHE_FLECHE_GAUCHE || touche == 0x08) curPiste = (curPiste + NB_COLONNES - 1) % NB_COLONNES;
  else if (touche == TOUCHE_FLECHE_DROITE || touche == 0x09) curPiste = (curPiste + 1) % NB_COLONNES;
  else if (touche == ' ' || touche == CORRECTION) {
    nouvelle = 0;
  }
  else if (touche == '#') {
    int d = note % 12;
    if (note == 0)            minitel.bip();
    else if (estDiese(note))  nouvelle = note - 1;       // retire le diese
    else if (d == 4 || d == 11) minitel.bip();           // pas de E# ni de B#
    else                      nouvelle = note + 1;
    noteSaisie = true;
  }
  else if (touche >= '1' && touche <= '7') {
    octaveCourante = touche - '0';
    if (note != 0) nouvelle = (octaveCourante + 1) * 12 + note % 12;
    afficherBandeauGrille();
    noteSaisie = true;
  }
  else {
    int d = demiTonLettre(touche);
    if (d < 0) return;                                   // touche sans effet
    nouvelle = (octaveCourante + 1) * 12 + d;
    noteSaisie = true;
  }

  // Deplacement du curseur : on redessine l'ancienne et la nouvelle case.
  if (curPas != ancienPas || curPiste != anciennePiste) {
    afficherCase(ancienPas, anciennePiste);
    afficherCase(curPas, curPiste);
    return;
  }

  if (nouvelle != note) {
    grille[curPas][curPiste] = (uint8_t)nouvelle;
    afficherCase(curPas, curPiste);
    marquerModifie();
  }
  if (noteSaisie && nouvelle != 0 && !lecture) ecouter(curPiste, (uint8_t)nouvelle);
}

void viderGrille() {
  for (int pas = 0; pas < NB_PAS; pas++)
    for (int p = 0; p < NB_PISTES; p++) grille[pas][p] = 0;
  for (int pas = 0; pas < NB_PAS; pas++) batterie[0][pas] = batterie[1][pas] = BAT_VIDE;
  marquerModifie();
  for (int pas = 0; pas < NB_PAS; pas++) afficherLignePas(pas);
}

// ============================================================================
// Memorisation
// ============================================================================

void memoriserEnceinte() {
  prefs.putBool("auto", true);
  prefs.putBytes("adr", adresseChoisie, 6);
  prefs.putString("nom", nomChoisi);
}

// ---- Emplacements de boucles (cles "b1" a "b9") ----------------------------

void lireEmplacements() {
  for (int n = 1; n <= NB_EMPLACEMENTS; n++) {
    char cle[4];
    snprintf(cle, sizeof(cle), "b%d", n);
    size_t taille = prefs.getBytesLength(cle);
    occupe[n - 1] = (taille == sizeof(Boucle) || taille == TAILLE_BOUCLE_V2 ||
                     taille == TAILLE_BOUCLE_V1);
    if (!occupe[n - 1]) continue;
    memset(&emplacements[n - 1], 0, sizeof(Boucle));
    prefs.getBytes(cle, &emplacements[n - 1], taille);
    Boucle& b = emplacements[n - 1];
    if (taille == TAILLE_BOUCLE_V1) b.volumeBatterie = 6;       // format sans batterie
    if (taille != sizeof(Boucle)) {                             // formats sans piste D2
      memset(b.batterie2, 0, sizeof(b.batterie2));
      b.volumeBatterie2 = 6;
    }
  }
}

// Ecrit la boucle de travail dans un emplacement, qui devient le courant.
// Une grille sans aucune note libere l'emplacement au lieu de l'occuper.
// Renvoie true si la boucle a ete sauvee, false si l'emplacement a ete libere.
bool sauverDansEmplacement(int n) {
  char cle[4];
  snprintf(cle, sizeof(cle), "b%d", n);

  bool vide = true;
  for (int pas = 0; pas < NB_PAS && vide; pas++)
    for (int p = 0; p < NB_PISTES; p++) if (grille[pas][p]) { vide = false; break; }
  for (int pas = 0; pas < NB_PAS; pas++) if (batterie[0][pas] || batterie[1][pas]) vide = false;

  if (vide) {
    if (occupe[n - 1]) prefs.remove(cle);
    occupe[n - 1] = false;
    if (prefs.getUChar("slot", 0) == n) prefs.remove("slot");
    emplacementCourant = 0;      // la grille vide n'est rattachee a aucun emplacement
    modifie = false;
    Serial.printf("Emplacement %d libere\n", n);
    return false;
  }

  capturerBoucle(emplacements[n - 1]);
  occupe[n - 1] = true;
  prefs.putBytes(cle, &emplacements[n - 1], sizeof(Boucle));
  prefs.putUChar("slot", n);
  emplacementCourant = n;
  modifie = false;
  Serial.printf("Boucle sauvee dans l'emplacement %d\n", n);
  return true;
}

// Charge un emplacement : tout de suite a l'arret, a la fin du tour en lecture.
void demanderChargement(int n) {
  chargementDemande = false;
  boucleEnAttente = emplacements[n - 1];
  emplacementDemande = n;
  if (lecture) {
    chargementDemande = true;
  } else {
    appliquerBoucle(boucleEnAttente);
    chargementFait = true;
  }
}

// Ancien format (une seule boucle, sauvegarde automatique) : relu une fois
// pour ne pas perdre le motif existant, tant qu'aucun emplacement n'est utilise.
void chargerMotif() {
  uint8_t copie[NB_PAS * NB_PISTES];
  if (prefs.getBytesLength("grille") == sizeof(copie)) {
    prefs.getBytes("grille", copie, sizeof(copie));
    memcpy((void*)grille, copie, sizeof(copie));
    for (int pas = 0; pas < NB_PAS; pas++) batterie[0][pas] = batterie[1][pas] = BAT_VIDE;   // pas de batterie a l'epoque
  }
  uint16_t t = prefs.getUShort("tempo", 120);
  tempoBpm = (t >= 60 && t <= 200) ? t : 120;
  if (prefs.getBytesLength("vol") == sizeof(niveauPiste)) {
    prefs.getBytes("vol", niveauPiste, sizeof(niveauPiste));
    for (int p = 0; p < 4; p++) if (niveauPiste[p] > 9) niveauPiste[p] = 9;
  }
  appliquerVolumes();
  uint8_t ondes[4];
  if (prefs.getBytesLength("onde") == sizeof(ondes)) {
    prefs.getBytes("onde", ondes, sizeof(ondes));
    for (int p = 0; p < 4; p++) formePiste[p] = ondes[p] < NB_ONDES ? ondes[p] : ONDE_CARREE;
  }
}

void oublierEnceinteEtRedemarrer() {
  lecture = false;
  prefs.putBool("auto", false);
  titre("ENCEINTE BLUETOOTH");
  ligne(3, "Redemarrage...");
  delay(500);
  ESP.restart();
}

// ============================================================================
// Suivi du repere de lecture, retarde pour coller au son
// ============================================================================

struct PasDate { uint8_t pas; uint32_t instant; };
const int TAILLE_FILE = 16;
PasDate file[TAILLE_FILE];
int fileTete = 0, fileQueue = 0;
int dernierPasVu = -1;

void viderFileRepere() {
  fileTete = fileQueue = 0;
  dernierPasVu = -1;
}

// A appeler souvent : note les changements de pas, et renvoie le pas a
// afficher maintenant (ou -1 s'il n'y a rien de nouveau).
int pasAAfficher() {
  int pas = pasCourant;
  uint32_t maintenant = millis();
  if (pas != dernierPasVu) {
    dernierPasVu = pas;
    file[fileQueue] = { (uint8_t)pas, maintenant };
    fileQueue = (fileQueue + 1) % TAILLE_FILE;
    if (fileQueue == fileTete) fileTete = (fileTete + 1) % TAILLE_FILE;  // file pleine
  }
  int resultat = -1;
  // On prend le pas echu le plus recent : si l'affichage est en retard,
  // les pas intermediaires sont sautes au lieu de s'accumuler.
  while (fileTete != fileQueue &&
         maintenant - file[fileTete].instant >= RETARD_AFFICHAGE_MS) {
    resultat = file[fileTete].pas;
    fileTete = (fileTete + 1) % TAILLE_FILE;
  }
  return resultat;
}

// ============================================================================
// Programme principal
// ============================================================================

enum Ecran { RECHERCHE, GRILLE, MENU, AIDE };
Ecran ecran = RECHERCHE;

void demarrerLecture() {
  pasSuivant = 0;
  echantillonsRestants = 0;
  viderFileRepere();
  lecture = true;
}

void arreterLecture() {
  lecture = false;
  viderFileRepere();
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);

  for (int n = 0; n < 128; n++) {
    float f = 440.0f * powf(2.0f, (n - 69) / 12.0f);
    incrementNote[n] = (uint32_t)(f * 4294967296.0f / FREQ_ECHANT_HZ);
  }
  for (int i = 0; i < 1024; i++) tableSinus[i] = sinf(2.0f * PI * i / 1024.0f);

  // Minitel
  // On passe a 4800 bauds si le Minitel l'accepte (1B et suivants).
  vitesseMinitel = minitel.searchSpeed();
  if (vitesseMinitel < 4800) {
    minitel.changeSpeed(4800);
    vitesseMinitel = minitel.searchSpeed();   // verification de la vitesse reelle
  }
  minitel.changeSpeed(vitesseMinitel);
  minitel.smallMode();
  minitel.extendedKeyboard();   // sans ce mode, les fleches ne sont pas transmises
  minitel.echo(false);

  // Enceinte memorisee ?
  prefs.begin("seq-bt", false);
  lireEmplacements();
  int dernier = prefs.getUChar("slot", 0);
  if (dernier >= 1 && dernier <= NB_EMPLACEMENTS && occupe[dernier - 1]) {
    appliquerBoucle(emplacements[dernier - 1]);
    emplacementCourant = dernier;
  } else {
    chargerMotif();            // ancien format, ou motif de test
  }
  bool reconnexion = prefs.getBool("auto", false);
  if (reconnexion && prefs.getBytes("adr", adresseChoisie, 6) == 6) {
    String nom = prefs.getString("nom", "");
    nettoyerNom(nomChoisi, nom.c_str());
    choixFait = true;
  } else {
    reconnexion = false;
  }

  Serial.println();
  Serial.println("=== Minitracker ===");
  Serial.printf("Enceinte memorisee : %s\n", reconnexion ? nomChoisi : "aucune");
  Serial.printf("Vitesse du Minitel : %d bauds\n", vitesseMinitel);

  ecranRecherche();

  // Bluetooth
  a2dp.set_ssid_callback(appareilTrouve);
  a2dp.set_data_callback_in_frames(produireAudio);
  a2dp.set_auto_reconnect(reconnexion);
  a2dp.set_volume(VOLUME_BT);
  a2dp.start();
}

void loop() {
  static int  versionAffichee   = 0;
  static bool etaitConnecte     = false;
  static bool confirmationVider = false;
  // Sommaire : action en attente d'un chiffre ou d'une confirmation.
  //   'S' sauver (attend 1-9)      'E' ecraser l'emplacement attenteN
  //   'C' charger attenteN malgre des modifications non sauvees
  //   'A' changer d'enceinte malgre des modifications non sauvees
  static char attente  = 0;
  static int  attenteN = 0;

  bool connecte = a2dp.is_connected();

  // ---- Changements d'etat de la liaison ------------------------------------
  if (connecte && !etaitConnecte) {
    Serial.println(">>> Enceinte connectee");
    if (choixFait) memoriserEnceinte();
    ecran = GRILLE;
    ecranGrille();
  }
  if (!connecte && etaitConnecte) {
    Serial.println(">>> Enceinte deconnectee");
    arreterLecture();
    ecran = RECHERCHE;
    ecranRecherche();
    versionAffichee = 0;        // forcer le reaffichage de la liste
  }
  etaitConnecte = connecte;

  // ---- Affichages periodiques ------------------------------------------------
  if (ecran == RECHERCHE && versionListe != versionAffichee) {
    versionAffichee = versionListe;
    afficherListe();
  }
  if (lecture) {
    int pas = pasAAfficher();
    if (pas >= 0 && ecran == GRILLE) poserRepere(pas - pas % pasDuRepere());
  }

  // ---- Changement de boucle --------------------------------------------------
  if (chargementDemande && !lecture) {       // lecture arretee avant la fin du tour
    chargementDemande = false;
    appliquerBoucle(boucleEnAttente);
    chargementFait = true;
  }
  if (chargementFait) {
    chargementFait = false;
    int ancien = emplacementCourant;
    emplacementCourant = emplacementDemande;
    modifie = false;
    prefs.putUChar("slot", emplacementCourant);
    if (ecran == GRILLE) {
      ecranGrille();
    } else if (ecran == MENU) {
      afficherEtatSommaire();
      if (ancien) afficherEmplacement(ancien);
      afficherEmplacement(emplacementCourant);
      messageSommaire("");
    }
  }

  // ---- Clavier ---------------------------------------------------------------
  unsigned long touche = minitel.getKeyCode();
  if (touche != 0) {
#if DEBUG_TOUCHES
    Serial.printf("Touche 0x%lX", touche);
    if (touche >= 32 && touche < 127) Serial.printf(" '%c'", (char)touche);
    Serial.printf("  (case pas %d, piste %d, note MIDI %d)\n",
                  curPas + 1, curPiste + 1,
                  curPiste < NB_PISTES ? grille[curPas][curPiste] : batterie[curPiste - COL_BATTERIE][curPas]);
#endif
    switch (ecran) {

      case RECHERCHE:
        if (touche >= '1' && touche <= '9') {
          int i = touche - '1';
          portENTER_CRITICAL(&verrouListe);
          bool valide = i < nbAppareils;
          if (valide) {
            memcpy(adresseChoisie, appareils[i].adresse, 6);
            strcpy(nomChoisi, appareils[i].nom[0] ? appareils[i].nom : "(sans nom)");
            choixFait = true;
          }
          portEXIT_CRITICAL(&verrouListe);
          if (valide) {
            Serial.printf("Enceinte choisie : %s\n", nomChoisi);
            ligne(3, String("Connexion a ") + nomChoisi + "...");
            ligne(4, "Cela peut prendre 10 a 20 secondes.");
          } else {
            minitel.bip();
          }
        } else if (touche == REPETITION) {
          oublierEnceinteEtRedemarrer();
        }
        break;

      case GRILLE:
        if (confirmationVider) {
          confirmationVider = false;
          if (touche == ANNULATION) viderGrille();
          afficherLigneEtat();
        } else if (touche == ANNULATION) {
          confirmationVider = true;
          ligne(2, "Tout vider ? ANNULATION = oui");
        } else if (touche == ENVOI) {
          if (lecture) { arreterLecture(); poserRepere(-1); }
          else         { demarrerLecture(); }
          afficherBandeauGrille();
        } else if (touche == SUITE && tempoBpm < 200) {
          tempoBpm += 5;
          marquerModifie();
          afficherBandeauGrille();
        } else if (touche == RETOUR && tempoBpm > 60) {
          tempoBpm -= 5;
          marquerModifie();
          afficherBandeauGrille();
        } else if (touche == 's' || touche == 'S') {
          if (emplacementCourant) {
            // Sauvegarde rapide dans l'emplacement d'origine
            sauverDansEmplacement(emplacementCourant);
            afficherBandeauGrille();           // l'etoile disparait
          } else {
            // Boucle jamais sauvee : on passe au sommaire pour choisir ou
            ecran = MENU;
            attente = 'S';
            ecranSommaire();
            messageSommaire("Sauver dans quel emplacement ? 1-9");
          }
        } else if (touche == SOMMAIRE) {
          ecran = MENU;
          attente = 0;
          ecranSommaire();
        } else if (touche == GUIDE) {
          ecran = AIDE;
          ecranAide();
        } else {
          toucheEdition(touche);
        }
        break;

      case AIDE:
        ecran = GRILLE;
        ecranGrille();
        break;

      case MENU: {
        char att = attente;
        attente = 0;
        char msg[41];

        if (touche == SOMMAIRE) {
          ecran = GRILLE;
          ecranGrille();

        } else if (touche == 's' || touche == 'S') {
          attente = 'S';
          messageSommaire("Sauver dans quel emplacement ? 1-9");

        } else if (touche >= '1' && touche <= '9') {
          int n = touche - '0';
          bool sauver = false;

          if (att == 'S') {
            if (occupe[n - 1] && n != emplacementCourant) {
              attente = 'E';
              attenteN = n;
              snprintf(msg, sizeof(msg), "Emplacement %d occupe : retapez %d", n, n);
              messageSommaire(msg);
            } else {
              sauver = true;
            }
          } else if (att == 'E' && n == attenteN) {
            sauver = true;
          } else if (!occupe[n - 1]) {
            minitel.bip();
            messageSommaire("Emplacement vide");
          } else if (modifie && !(att == 'C' && n == attenteN)) {
            attente = 'C';
            attenteN = n;
            snprintf(msg, sizeof(msg), "Modifs non sauvees : retapez %d", n);
            messageSommaire(msg);
          } else {
            demanderChargement(n);
            if (lecture) {
              snprintf(msg, sizeof(msg), "Boucle %d a la fin du tour...", n);
              messageSommaire(msg);
            }
          }

          if (sauver) {
            int ancien = emplacementCourant;
            bool sauvee = sauverDansEmplacement(n);
            afficherEtatSommaire();
            if (ancien && ancien != n) afficherEmplacement(ancien);
            afficherEmplacement(n);
            snprintf(msg, sizeof(msg), sauvee ? "Boucle sauvee dans l'emplacement %d"
                                              : "Grille vide : emplacement %d libere", n);
            messageSommaire(msg);
          }

        } else if (touche == ANNULATION) {
          if (modifie && att != 'A') {
            attente = 'A';
            messageSommaire("Boucle non sauvee ! ANNULATION = oui");
          } else {
            oublierEnceinteEtRedemarrer();
          }

        } else {
          messageSommaire("");
        }
        break;
      }
    }
  }

  // LED : fixe si connecte a l'arret, battement sur les temps en lecture
  if (!connecte)      digitalWrite(LED_PIN, LOW);
  else if (!lecture)  digitalWrite(LED_PIN, HIGH);
  else                digitalWrite(LED_PIN, (pasCourant % 4 == 0) ? HIGH : LOW);

  delay(2);
}
