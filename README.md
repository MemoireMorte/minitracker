# Minitracker

Séquenceur pas à pas pour la carte [**Minitel Wifi V2**](https://github.com/MemoireMorte/Esp32-Minitel-dev-kit) : on compose une boucle de 16 pas sur 4 pistes mélodiques et 2 pistes de batterie au clavier du Minitel, et le son sort sur une enceinte Bluetooth.

Tout le programme tient dans `minitracker.ino`.

## Installation

1. Bibliothèques Arduino : [ESP32-A2DP](https://github.com/pschatzmann/ESP32-A2DP) et [Minitel1B_Hard](https://github.com/eserandour/Minitel1B_Hard).
2. Carte **ESP32 Dev Module**, schéma de partition **Huge APP (3MB No OTA/1MB SPIFFS)**.
3. Téléverser avec un module FTDI (PROG maintenu + RESET), voir le README de la carte.
4. Alimenter la carte par le micro-USB ou par le Minitel. Sur le FTDI seul, le Bluetooth provoque un redémarrage par sous-tension.

## Première utilisation : appairer l'enceinte

1. Mettre l'enceinte en mode appairage et allumer la carte.
2. Le Minitel liste les appareils audio trouvés. Taper le chiffre de l'enceinte ; la connexion prend 10 à 20 secondes.
3. L'enceinte est mémorisée : aux démarrages suivants, la reconnexion est automatique.

Pour changer d'enceinte : **Sommaire**, puis **Annulation**. Sauvegardez d'abord la boucle en cours : le changement redémarre la carte.

## La grille

Les pas défilent de haut en bas, une colonne par piste. Les numéros des pas 1, 5, 9 et 13 (les temps) sont en vidéo inverse. Les colonnes `P1` à `P4` sont les pistes mélodiques, `D1` et `D2` les pistes de batterie. Au-dessus de la grille, la ligne `ONDE` donne la forme d'onde de chaque piste mélodique et la ligne `VOL` le volume de chacune des six pistes. Le bandeau affiche la boucle en cours, le tempo et l'octave courante.

| Touche | Action |
|---|---|
| Flèches | Déplacer le curseur |
| `A` à `G` | Poser une note (notation américaine : A = la, C = do) |
| `#` | Ajouter ou retirer le dièse (sauf sur E et B) |
| `1` à `7` | Octave de la case, et des notes suivantes |
| Espace ou **Correction** | Vider la case |
| `+` et `-` | Volume de la piste sous le curseur (0 = muette, 9 = maximum) |
| `*` | Forme d'onde de la piste sous le curseur : `CAR` carrée, `SCI` dent de scie, `TRI` triangle, `SIN` sinus |
| `K` `G` `N` `C` `O` `T` | Sur les pistes `D1` et `D2` : kick (`KIK`), kick gabber (`GAB`), caisse claire (`SNR`), hi-hat fermé (`CHH`), hi-hat ouvert (`OHH`), tom (`TOM`) |
| **Envoi** | Lancer / arrêter la lecture en boucle |
| `S` | Sauver la boucle dans son emplacement d'origine |
| **Suite** / **Retour** | Tempo +5 / −5 BPM (60 à 200) |
| **Annulation**, deux fois | Vider toute la grille |
| **Guide** | Écran d'aide |
| **Sommaire** | Boucles sauvegardées et enceinte |

À l'arrêt, chaque note ou son de batterie posé est joué une fois.

## Boucles et sauvegarde

La boucle affichée dans la grille est une copie de travail : **rien n'est sauvegardé automatiquement**. On peut modifier librement, tester des variantes, et tout est perdu si on éteint sans sauver. Le bandeau indique l'emplacement d'origine, suivi d'une étoile si la boucle a été modifiée depuis (`BOUCLE 3*`).

Dans la grille, `S` sauve directement dans l'emplacement d'origine et l'étoile disparaît. Si la boucle n'a encore jamais été sauvée, `S` ouvre le sommaire et demande dans quel emplacement l'enregistrer.

L'écran **Sommaire** liste 9 emplacements, avec pour chacun son tempo et une frise des pas qui contiennent au moins une note ou un son de batterie.

| Touche (écran Sommaire) | Action |
|---|---|
| `1` à `9` | Charger la boucle de cet emplacement |
| `S` puis `1` à `9` | Sauver la boucle en cours dans cet emplacement |
| **Sommaire** | Retour à la grille |
| **Annulation** | Changer d'enceinte |

- **Pendant la lecture**, la boucle choisie prend effet à la fin du tour en cours, sans rupture de rythme. On peut ainsi enchaîner des boucles à la main.
- **Confirmations** : il faut retaper le chiffre pour écraser un emplacement déjà occupé par une autre boucle, ou pour charger une boucle alors que la boucle en cours a des modifications non sauvées.
- **Libérer un emplacement** : sauver une grille vide (sans aucune note) dans un emplacement le libère. Depuis la grille : **Annulation** deux fois pour tout vider, puis **Sommaire**, `S` et le chiffre.
- Au démarrage, la carte recharge le dernier emplacement chargé ou sauvé.
- Chaque boucle garde sa grille, sa batterie, son tempo, ses volumes et ses formes d'onde.
- Les boucles sauvées avant l'ajout des pistes de batterie restent lisibles ; elles se chargent avec les pistes manquantes vides.

## Réglages dans le code

| Constante | Rôle |
|---|---|
| `VOLUME_BT` | Volume général envoyé à l'enceinte (0 à 127) |
| `AMPLITUDE` | Niveau d'une voix avant mixage |
| `NIVEAU_BATTERIE` | Niveau général de la batterie par rapport aux pistes mélodiques |
| `GABBER_NIVEAU`, `GABBER_SATURATION` | Niveau et saturation du kick gabber |
| `RETARD_AFFICHAGE_MS` | Retard du repère de lecture, pour compenser la latence de l'enceinte |
| `DEBUG_TOUCHES` | À 1, affiche le code de chaque touche sur le moniteur série |

## Limites connues

- Les deux pistes de batterie partagent les mêmes voix : le même son posé sur le même pas des deux pistes ne joue qu'une fois, au volume de `D2`.
- La latence du Bluetooth (100 à 300 ms) interdit le jeu en direct : c'est un séquenceur, pas un clavier.
- Les petites enceintes restituent mal les notes graves ; monter la basse d'une octave aide plus que le volume.
- À 1200 bauds (Minitel 1), le repère de lecture saute un pas sur deux au-delà de 135 BPM.

## Notes de conception

- **Horloge dans le flux audio** : le séquenceur avance en comptant les échantillons produits pour le Bluetooth (44,1 kHz), pas avec `millis()`. Le tempo reste exact quoi que fasse l'affichage.
- **Moteur sonore** : 4 voix à accumulateur de phase, forme d'onde au choix par piste (carrée, dent de scie, triangle, sinus), attaque de 3 ms puis décroissance. Une case vide laisse la note précédente s'éteindre.
- **Affichage économe** : le positionnement du curseur utilise la séquence Vidéotex courte (3 octets) au lieu de `moveCursorXY()` de la bibliothèque (6 à 8 octets). Avec la version longue, le repère de lecture décrochait dès 120 BPM à 1200 bauds.
- **Vitesse** : le croquis passe le Minitel à 4800 bauds quand il l'accepte (1B et suivants), puis vérifie la vitesse réelle.
- **Clavier** : les flèches ne sont transmises qu'en mode clavier étendu (`extendedKeyboard()`).
- **Appairage** : la bibliothèque appelle une fonction pour chaque appareil audio trouvé ; on y remplit la liste et on n'accepte que l'adresse choisie. Changer d'enceinte passe par un redémarrage, plus fiable qu'une déconnexion à chaud.
- **Batterie** : sons calculés, sans échantillons. Kick et tom sont des sinus dont la fréquence glisse vers le grave ; le kick gabber part de plus haut, dure plus longtemps et passe dans une forte saturation (`GABBER_SATURATION`, `GABBER_NIVEAU`) ; la caisse claire mêle bruit blanc et un ton court ; les hi-hats sont du bruit filtré passe-haut, avec un déclin court ou long. Chaque son a sa propre voix et peut résonner sous le suivant ; les deux pistes servent à superposer deux sons différents sur un même pas.
- **Sauvegarde** : `Preferences` (mémoire flash), espace de noms `seq-bt` (nom d'origine du projet, conservé pour ne pas perdre les boucles existantes), clés `b1` à `b9` (108 octets par boucle ; les formats antérieurs de 74 et 92 octets restent lus). Les 9 emplacements sont copiés en mémoire vive au démarrage : charger une boucle ne lit pas la flash, et l'échange se fait dans le rappel audio, au premier pas du tour.

