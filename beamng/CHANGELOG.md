# Sonic Beam — journal des versions

Versions au format `vMAJEUR.MINEUR.PATCH` (le numéro est dans `beamng/VERSION`, le tag git est `vX.Y.Z`) :
- **PATCH** (3e nombre) : petite mise à jour, correction ou petit ajout ;
- **MINEUR** (2e nombre) : mise à jour plus importante ;
- **MAJEUR** (1er nombre) : grosse mise à jour.

## v0.11.0
- Collisions avec les véhicules (trafic, voitures garées, la sienne) : chaque image, le trajet de Sonic est testé contre la boîte de chaque véhicule, avec 0,08 s d'avance sur sa course (la collision normale de BeamNG arrête son corps léger dès le contact : le choc doit partir juste avant, à pleine vitesse). À l'impact, il donne au véhicule l'élan d'une masse de 1000 kg × « Impact force » : le véhicule est projeté, et poussé plus fort autour du point d'impact pour que la carrosserie s'écrase (extension de véhicule `sonicfrontiers_impact`, qui donne aussi la masse du véhicule). Éclair et éclats électriques de Frontiers au point d'impact, bang supersonique au-delà de 120 km/h.
- Labo, nouvel onglet **Impact** : ce que fait Sonic au choc (une case cochée à la fois) :
  - **Sonic loses speed by the mass of the car** (par défaut) : il perd d'autant plus de vitesse que la voiture est lourde ;
  - **Sonic keeps his speed and plows through** : il garde sa vitesse ;
  - **Sonic bounces off the car** : il rebondit ;
  - **Off** : seule la collision normale de BeamNG.
  Curseurs « Impact force » (×1) et « Car damage » (×1, 0 = poussée sans déformation).

## v0.10.2
- Nage : Sonic nage debout dans l'eau jusqu'au cou (animation de marche de Frontiers coupée à la surface) ; seule sa tête dépasse, au lieu d'un corps allongé qui semblait flotter dans l'air.
- Passage de la terre à l'eau sans ralentir : assez rapide, il court à la surface même en eau peu profonde, en glissant juste au-dessus (avant, il traversait l'eau peu profonde en marchant dedans, la résistance de l'eau de BeamNG le freinait et il finissait par nager).

## v0.10.1
- Nage : Sonic est coupé à la surface de l'eau (le moteur de rendu ne dessine plus ce qui est dessous, ombre comprise), et placé pour que la ligne d'eau passe à mi-corps quelle que soit la pose, donc il nage à moitié immergé sous tous les angles au lieu de paraître sous l'eau ou en l'air ; il tourne doucement au lieu de pivoter dans tous les sens ; la flottaison est plus douce.
- Sortie de l'eau : le fond est cherché depuis au-dessus de Sonic. Sur une plage au ras de l'eau, il croyait encore nager ; il reprend maintenant sa course dès qu'il a pied.
- Atterrissage sur l'eau après un saut : avec au moins 85 % de la vitesse de course sur l'eau, il repart directement en courant au lieu de nager ; une chute sur l'eau est arrêtée net (en plongeant profond, la résistance de l'eau de BeamNG lui faisait perdre toute sa vitesse).

## v0.10.0
- Sonic et l'eau : quand il n'a pas pied, assez rapide il court sur l'eau (porté à la surface, animations de course et de boost, pas « eau » de Frontiers), sinon il nage (enfoncé jusqu'à la poitrine, animation de plongée `dive_loop` de Frontiers, dessiné à moitié dans l'eau, lent, sans boost ni saut) jusqu'à la rive. Sur un haut-fond, il court normalement.
- La hauteur de l'eau vient des objets d'eau de la carte (mers, lacs, bassins) : le test de BeamNG par point du corps ne voit rien à grande vitesse.
- Labo, nouvel onglet **Water** : « Run on water » (case), « Water run speed » (80 km/h), « Swim speed » (8 km/h).

## v0.9.2
- Saut en boule : une fois la boule formée, seule la coque de Frontiers est dessinée, comme dans le jeu ; le corps de Sonic roulé en boule est caché (ses semelles et ses piquants dépassaient de la coque en tournant). Il réapparaît dès qu'il se déroule ; l'ombre au sol reste.

## v0.9.1
- Aura du boost couchée dans le sens de la course, comme sur les illustrations de Sonic : un bouclier arrondi juste devant son visage, qui l'englobe et s'étire vers l'arrière en traînées d'énergie qui s'effacent. Trois couches emboîtées, lumineuses à l'avant, de plus en plus transparentes vers l'arrière ; la texture d'énergie de Frontiers défile vers l'arrière, plus vite quand il accélère. L'aura suit la vraie direction du mouvement (pentes, sauts), pas seulement son regard, et s'allonge avec la vitesse.
- Labo, onglet Aura : « Aura thickness » règle la grosseur, nouveau curseur « Aura length » (longueur vers l'arrière, ×1) ; case « Aura follows body shape » pour revenir à l'aura collée au corps (décochée par défaut).

## v0.9.0
- Aura du boost plus bleue : un bleu électrique profond au lieu du bleu ciel qui tournait au blanc sur les zones claires ; les traînées d'énergie de la texture restent bleues.
- Éclairs sur le corps : quand l'aura est allumée (boost, grande vitesse), de petits arcs électriques (texture d'éclair de Frontiers) crépitent sur les bras, les jambes et les piquants de Sonic. Chacun est accroché à un os, donc il suit ses mouvements ; il y en a plus quand il va vite.
- Labo, nouvel onglet **Aura** : « Aura thickness » (épaisseur autour du corps, ×1), « Aura intensity » (luminosité, ×1), « Aura lightning » (quantité d'éclairs sur le corps, ×1, 0 pour les couper).
- Correction : en Direct3D 11 comme en Direct3D 12, le défilement de la texture de l'aura et sa conversion de couleur pour l'écran étaient restés dans un commentaire du code ; ils sont de nouveau appliqués.

## v0.8.2
- Labo, onglet Boost : « Trail start fade », la longueur du fondu au départ de la traînée derrière Sonic (0 m = départ net, 1 m par défaut, jusqu'à 4 m au curseur).
- Labo et menu O : chaque valeur affichée à côté d'un curseur se modifie à la main. Un clic sur le chiffre ouvre une case, on tape la valeur, Entrée (ou un clic ailleurs) la garde. Les valeurs tapées peuvent dépasser les limites du curseur (par exemple Jump height ×10).

## v0.8.1
- Boule du saut : plus de petites lumières blanches qui tournent autour. Le halo (« Ball glow ») est maintenant une lueur bleue ronde, douce et fixe, sans texture ni rotation ; l'aura du boost (son liseré clair à rayures) s'efface pendant la boule.
- Traînée du boost : elle part juste derrière Sonic et apparaît en fondu sur son premier mètre, au lieu d'un bord net collé à lui ; quand elle pointe vers la caméra (dérapage, virage serré), elle se couche à plat au lieu de se mettre de champ en une ligne d'épaisseur changeante.

## v0.8.0
- Saut en boule comme dans Sonic Frontiers : au saut, Sonic passe par `jump_s` puis se roule en boule (`ball_loop`) et tourne sur lui-même dans la coque de spin de Frontiers (`chr_sonicspin.model`, son squelette et sa boucle `spin_normal_loop`), avec un halo bleu (aura + lueur). Il se déroule à l'atterrissage. Un saut sans boule (chute d'un rebord) reste comme avant.
- Labo, nouvel onglet **Jump** :
  - trois cases (une seule cochée à la fois) : boule pendant tout le saut sans figures ; boule à la montée puis figures/chute après le sommet ; boule avec une figure quand on tient une direction en l'air (A gauche, D droite, S vers le haut) ;
  - « Ball spin speed » (tours par seconde, 3 par défaut), « Ball glow » (×1), « Jump height » (×1, la hauteur suit le réglage).
- Labo, onglet Boost : « Speed blur strength » (force du flou de vitesse, ×1 par défaut, 0 = sans flou).

## v0.7.1
- BeamNG 0.40.1 : le piéton de BeamNG est maintenant un vrai personnage, et le jeu réaffiche ses morceaux de lui-même (changement de caméra, réinitialisation après une instabilité). Sonic Beam le re-cache à ces moments-là et le revérifie deux fois par seconde : plus de mannequin derrière Sonic.

## v0.7.0
- Menu O, réglage « Sonic » à 3 choix (enregistré avec Close) :
  - **Auto** (par défaut) : comme avant, Sonic en mode Libre seulement, en partant à pied ; la carte BeamNG.Earth compte maintenant comme du mode Libre (elle a sa propre disposition d'écran, `beamngEarth`) ;
  - **Always** : Sonic remplace le piéton dans tous les modes (carrière, missions, scénarios…) ; hors mode Libre il ne sort jamais le joueur de sa voiture, Sonic arrive quand on descend (F) ;
  - **Off** : le piéton normal de BeamNG partout.

## v0.6.2
- Installation hors Melty : `install.bat` / `uninstall.bat` (avec `setup.ps1`) dans le paquet. L'installeur trouve BeamNG.drive et Sonic Frontiers dans les bibliothèques Steam (ou demande leurs dossiers), sauvegarde tout fichier qu'il remplacerait, installe le mod et note ce qu'il a fait dans `%LOCALAPPDATA%\SonicBeam\install.json` ; le désinstalleur retire tout et remet les fichiers sauvegardés.
- Readme du paquet : installation, désinstallation et installation à la main. `Bin64/SonicBeam/VERSION` indique la version installée.
- Dépôt public GitHub : seulement Sonic Beam (le projet Forza Horizon 5 est retiré), README principal en anglais avec captures.

## v0.6.1
- Readme dans le paquet (racine de l'archive et `Bin64/SonicBeam/README.md`) : présentation, commandes, versions des jeux prises en charge (BeamNG.drive 0.40, Sonic Frontiers PC à jour), crédits et licences.

## v0.6.0
- Les effets du boost suivent la vitesse de Sonic : longueur et intensité de la traînée, éclairs (et ceux du lancement) et aura bleue montent avec la vitesse, jusqu'à 100 % des réglages à la vitesse de référence (la vitesse max du menu O, ou 500 km/h si la limite est coupée).
- Labo, onglet Boost : case « Effects follow Sonic's speed » et curseur « Speed influence » (0 = aucun effet de la vitesse, 0,75 par défaut = les effets partent de 25 %, 1 = partent de 0), avec la vitesse de référence affichée.
- Labo réorganisé en onglets : Sound, Shadow, Boost, Animation ; « Close and save » reste en bas.

## v0.5.0
- Traînée du boost façon Power Boost de Frontiers :
  - un faisceau bleu électrique qui suit le vrai chemin de Sonic (virages, sauts), large contre lui et effilé vers l'arrière, qui recule et s'éteint quand le boost s'arrête ;
  - les traits lumineux de la traînée de Sonic (`ef_y01_so_locus01`) qui défilent dessus ;
  - des arcs électriques le long de la traînée (`ef_n01_com_lightning04`, qui se redessinent ~14 fois par seconde) et des éclairs qui crépitent autour de Sonic (planche de 16 éclairs `ef_i01_com_lightning02_anim`).
- Au lancement du boost : une salve d'éclairs et des boules lumineuses bleues qui s'envolent (`ef_n01_com_glowlight13`), comme le `pow_boost_impact` de Frontiers.
- Labo : section « Boost trail » avec « Trail length », « Trail intensity » et « Lightning » (0 coupe les éclairs).
- Outil de développement `ddsview` : décode une texture BC6H/BC7 de Frontiers en image pour la regarder.

## v0.4.2
- Labo : les valeurs affichées gardent leur symbole % (« Shadow opacity 55 % » au lieu de « 55 ») ; l'affichage n'interprète plus le texte comme un format.

## v0.4.1
- Labo : le bouton « Close » devient « Close and save » (il enregistrait déjà, c'est maintenant explicite).
- Bang au mur du son supprimé (réglage, ligne du labo et déclenchement). Le bang au lancement d'un boost rapide reste.

## v0.4.0
- Ombre dynamique de Sonic : sa silhouette (animée) aplatie sur le sol dans la direction du soleil de BeamNG (azimut et hauteur lus dans le ciel du jeu), sur la pente mesurée sous ses pieds, qui s'efface quand il saute ; chaque pixel n'est assombri qu'une fois (stencil). Soleil bas ou nuit : ombre ronde douce.
- Labo : section « Shadow » avec « Shadow opacity » (0 à 100 %, 55 % par défaut).
- Rendus D3D11 et D3D12 : tampon de profondeur avec stencil ; 9 shaders vérifiés au build.

## v0.3.3
- Vent de vitesse de Sonic Frontiers (`sn_relative_wind`) : en boucle, de 80 à 600 km/h, plus fort et plus aigu avec la vitesse, au moins aux 3/5 pendant le boost.
- Bang supersonique (`sn_sonic_boom`) : au mur du son (1 235 km/h, réarmé sous 90 %) et au lancement d'un boost à 200 km/h ou plus.
- Labo : « Wind » (dB) ; « Sonic boom at the sound barrier » et « Sonic boom on a fast boost », chacun avec on/off et sa vitesse.
- Le labo s'ouvre à gauche de l'écran (il passait sous Sonic).

## v0.3.2
- Animations plus rapides, collées à la vitesse : course 1× à 32 km/h, plafond 3,2× (avant 2,1×) ; boost 1× à 60 km/h, plafond 4,5× (avant 2,6×) ; marche plafond 2×. Le plafond reste doux (pas de membres qui s'affolent).
- Labo : section « Animation » avec « Animation speed » (multiplie la cadence, ×0,5 à ×3) et « Animation speed limit » (multiplie le plafond, ×0,5 à ×3), valeur affichée et bouton ×1.

## v0.3.1
- **Labo de paramètres** (« Sonic Beam Lab ») : bouton « Lab » dans le menu O. Premiers réglages, en décibels avec la valeur affichée à côté et un bouton « 0 dB » :
  - volume des bruits de pas ;
  - volume général de tous les sons du mod (Sonic et son environnement).
  De −40 dB (coupé) à +12 dB, enregistrés à la fermeture du labo, appliqués en direct.
- Sonic et le HUD se cachent quand un écran de BeamNG est ouvert (menu pause, menu principal…), au lieu d'être dessinés par-dessus.
- Menu O et labo placés plus bas, sous le texte d'aide du démarrage.

## v0.3.0
- Bruits de pas de Sonic Frontiers (banque `se_rangers_player_sonic.acb` du joueur) :
  - pas de marche (`sn_walk_…`) et de course (`sn_run_…`), calés sur ses pieds : les contacts sont calculés dans les animations (orteils `Toe_L`/`Toe_R`, 2 par cycle) ; hauteur légèrement variée à chaque pas ;
  - selon le sol que BeamNG signale sous Sonic : béton (asphalte, roche, pavés, glace…), terre (terre, gravier, boue), herbe, sable, neige, eau, métal ;
  - atterrissage selon le sol (`sn_land_…`), freinage (`sn_brake`) quand il s'arrête lancé ;
  - pas de bruits de pas pendant le boost, comme dans Frontiers.
- Chaque banque de sons n'est lue qu'une fois au chargement.

## v0.2.3
- Correctif : en chargeant une carte depuis le menu, Sonic restait dans la voiture (BeamNG annonce le mode Libre un peu après l'apparition de la carte). Le mod attend maintenant 3 s avant de renoncer à mettre le joueur à pied.
- Raccourci de test : options de lancement Steam sans guillemets (le lanceur de BeamNG plante avec).

## v0.2.2
- Anneaux mis de côté pour l'instant : plus d'anneaux au sol, de récompenses ni de compteur d'anneaux. Le compteur de vitesse prend sa place en haut à gauche. Tout le code reste là, derrière l'interrupteur `ringsEnabled` (sheets/tuning.json, 0 → 1 pour les remettre).
- Test Melty : la vérification « ring-collected » est retirée tant que les anneaux sont coupés.

## v0.2.1
- Raccourci de test `Lancer-SonicBeam-test.bat` : lance BeamNG sans arguments, donc sans l'avertissement de Steam (le dossier de test se règle une fois dans les options de lancement Steam).

## v0.2.0
- Effet de boost de Sonic Frontiers :
  - flou de vitesse radial : l'image file vers Sonic, de plus en plus fort avec la vitesse et le boost ; mini-carte et barre d'outils de BeamNG restent nettes ;
  - aura bleue : le vrai maillage d'aura de Frontiers (`chr_sonic_aura.model`) sur la pose de Sonic, avec la texture `ef_y02_so_aura01` qui défile (matériau `SonicAura` recréé) ;
  - éclair et onde de choc au lancement du boost (textures `boost_impact` de Frontiers).
- Les deux rendus (D3D11 et D3D12) partagent un seul code de shaders, vérifié à la compilation (`tools/hlslcheck.cpp`).
- Outils de développement : `pacstrings` (noms dans une archive PAC), `aurainfo` (squelette et normales de l'aura).

## v0.1.4
- Menu (touche O) : bouton « Unstuck: back to the last safe spot » pour remettre Sonic debout quand il est coincé (rien d'automatique).
- Après une réinitialisation du corps de marche, la traînée réduite de Sonic est réappliquée.
- La version vient du fichier `VERSION` (build et journal du rendu).

## v0.1.3
- Rendu de Sonic par `version.dll` (Bin64) et liaison UDP locale avec le script Lua ; Direct3D 11 et 12.
- Compteur de vitesse, boost sans plafond avec limite réglable, menu en jeu (touche O).
- Animations Frontiers dont la cadence suit la vitesse et plafonne en douceur.
- Haute vitesse : glisse, sans traînée d'air, accroche au sol ; saut propre à Sonic.
- Sonic ne prend la main qu'en mode Libre (pas dans le tutoriel, la carrière, les scénarios ni les missions).

## v0.1.0 – v0.1.1
- Premières versions envoyées sur Melty (liaison FFI, abandonnée : BeamNG l'interdit aux mods).
