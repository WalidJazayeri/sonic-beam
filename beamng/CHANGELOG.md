# Sonic Beam — journal des versions

Versions au format `vMAJEUR.MINEUR.PATCH` (le numéro est dans `beamng/VERSION`, le tag git est `vX.Y.Z`) :
- **PATCH** (3e nombre) : petite mise à jour, correction ou petit ajout ;
- **MINEUR** (2e nombre) : mise à jour plus importante ;
- **MAJEUR** (1er nombre) : grosse mise à jour.

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
