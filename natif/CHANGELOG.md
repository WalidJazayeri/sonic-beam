# Sonic Beam natif — journal des versions

Le mode natif (dossier `natif/`) a ses propres versions : le numéro est dans `natif/VERSION` et le tag git est `natif-vX.Y.Z`, distinct des tags `vX.Y.Z` du mode superposé (`beamng/`). Les règles sont les mêmes :
- **PATCH** (3e nombre) : petite mise à jour, correction ou petit ajout ;
- **MINEUR** (2e nombre) : mise à jour plus importante ;
- **MAJEUR** (1er nombre) : grosse mise à jour.

## natif-v0.5.0
- Effets du boost en natif, refaits avec les outils de BeamNG (le superposé avait ses propres shaders par-dessus l'image) :
  - **flou de vitesse radial** vers Sonic : notre propre shader, branché dans les effets d'écran de BeamNG (Direct3D 12) ; il monte de 150 à 600 km/h et Sonic reste net au centre ;
  - **aura bleue** : le vrai modèle d'aura de Frontiers, ajouté au modèle de Sonic en deux couches qui suivent ses os, avec un matériau lumineux additif qui défile ; son intensité suit la vitesse ;
  - **éclairs** qui crépitent sur son corps (accrochés à 24 os grâce aux positions exportées par le convertisseur) et autour de lui ;
  - **traînée du Power Boost** : faisceau bleu, traits et arcs électriques, reconstruits à chaque image ;
  - **flash au départ du boost** (et lors d'un choc) : lueur, onde, orbes, gerbe d'éclairs, et une vraie lumière bleue qui éclaire le décor (nouveau par rapport au superposé).
- Contrairement au superposé, les effets sont cachés par les murs et le décor.
- Le convertisseur extrait l'aura et les textures d'effets de Frontiers (aucun fichier de Frontiers dans le dépôt) et écrit les matériaux des effets (24 niveaux d'intensité par effet).
- Lab : onglets **Boost** (longueur et intensité de la traînée, éclairs, début de la traînée, effets selon la vitesse, force du flou, lumière du boost) et **Aura** (intensité, éclairs sur le corps).
- Testé dans BeamNG (Gridmap V2) : flou, aura et éclairs forcés à 600 km/h, puis boost réel de 0 à 350 km/h avec flash, lumière, traînée, aura et flou qui montent avec la vitesse ; aucune erreur dans le journal ; environ 1,5 ms par image en boost.
- À savoir : l'aura n'a pas le liseré lumineux exact du superposé (pas de shader sur mesure pour elle) ; sa luminosité change par paliers.

## natif-v0.4.0
- Menu Sonic Beam sur la touche **O**, comme dans le superposé : mode de Sonic (Auto / Always / Off), limite de vitesse et vitesse max, accélération du boost, compteur de vitesse (en haut à gauche, caché quand un écran de BeamNG est ouvert), boutons Unstuck et Lab.
- **Lab**, avec les onglets qui existent en natif : Sound (volumes des pas, de tous les sons et du vent ; bang supersonique et sa vitesse), Water, Wall, Impact, Jump (style de saut, vitesse de rotation de la boule, hauteur) et Animation (vitesse et plafond). Chaque valeur se tape au clavier en cliquant dessus, même hors de la plage du curseur, et a son bouton de retour au défaut. Les réglages s'appliquent en direct et sont enregistrés dans `settings/sonicnative.json` (un fichier à part du superposé) à chaque fermeture du menu ou du Lab. Les onglets d'effets du superposé (ombre, boost, aura) reviendront avec ces effets.
- **Unstuck** dans le menu et sur la touche **U** (libre à pied ; seul le camion-benne s'en sert, au volant) : remet Sonic debout sur son dernier point sûr, comme le superposé. Les deux touches se changent dans les commandes de BeamNG.
- Testé dans BeamNG : touches O et U enregistrées, menu, Lab et compteur affichés, Unstuck ramène Sonic projeté à 15 m sur son dernier point sûr, réglages enregistrés et relus.

## natif-v0.3.1
- Course sur les murs corrigée : au-dessus d'environ 150 km/h, elle lâchait au bout de 0,03 s. Le contact avec le mur n'était cherché qu'une fois par image : à 240 km/h, Sonic était déclaré « contre le mur » encore à 5 m de lui, hors de la portée qui le garde sur le mur à l'image suivante, et il percutait le mur à pleine vitesse puis rebondissait. Le contact est maintenant cherché 250 fois par seconde dans le calcul physique, et juste avant le contact sa vitesse est tournée vers le haut ou le long du mur (il n'arrive plus qu'à 3 m/s contre lui) au lieu de s'y écraser.
- Mesuré sur un mur de Gridmap V2 : de face à 230 km/h, il monte de 33 à 42 m jusqu'en haut et passe par-dessus ; à 150 km/h, environ 30 m ; en biais, il court le long du mur jusqu'à sa fin.
- Le mode superposé (en pause) a la même physique, donc le même défaut ; il n'est pas corrigé.

## natif-v0.3.0
- Toutes les animations du mode superposé : marche, course, boost avec inclinaison à gauche et à droite, **drift** à gauche et à droite (boost tenu au-dessus de 60 km/h en tournant fort), départ de saut, **boule de saut**, **figures en l'air** (après 0,8 s en l'air sans saut), atterrissage (0,45 s), course sur les murs (montée, gauche, droite) et **nage**. Elles sont choisies par la même logique que le superposé (priorités, délais, inclinaison de 16° dans les virages), portée en Lua avec ses nombres dans `lua/common/sonicnative/anim.lua`.
- La boule de saut est la vraie coque de Frontiers : un second modèle qui tourne autour de Sonic pendant le saut, Sonic étant caché une fois la boule formée, comme dans le superposé.
- Le convertisseur exporte les 19 animations et la coque de la boule (`--no-ball` pour s'en passer, `--out <dossier>` pour écrire ailleurs et inspecter). Le modèle passe de 10 à 18 Mo.
- Sons de drift (au début de chaque drift, à chaque changement de côté) et de figure ; pas de bruits de pas pendant le boost, le drift et l'état « rapide », comme dans le superposé.
- Testé dans BeamNG (Gridmap V2) : drift, boule de saut puis atterrissage, figures pendant une chute, nage dans le bassin de la carte (Sonic à moitié dans l'eau, l'eau de BeamNG cache le bas du corps).
- À savoir : sans fondu entre deux animations (BeamNG n'en propose pas pour cet objet), le passage d'une animation à l'autre est net. La course sur les murs démarre mais s'arrête au bout de 0,03 s : c'est la physique du mur, à corriger à part.

## natif-v0.2.1
- Saut à grande vitesse corrigé : plus Sonic allait vite, plus son saut s'écrasait (environ 3,2 m à l'arrêt, 1,9 m à 300 km/h, 0,8 m à 560 km/h, au point qu'on n'entendait que le son du saut). La poussée du saut vise maintenant la vitesse de saut réelle quoi que le sol en prenne, et pendant tout le saut sa hauteur suit l'arc d'un saut à l'arrêt : ce que BeamNG retire au corps de marche rapide lui est rendu. Mesuré sur Gridmap V2, en boost, terrain plat : 3,4 m à l'arrêt, 3,1 m à 300 km/h, 2,7 m à 560 km/h, environ 1,2 s en l'air.
- Le mode superposé (en pause) a la même physique, donc le même défaut ; il n'est pas corrigé.

## natif-v0.2.0
- Sons de Sonic par l'audio de BeamNG : pas selon le sol (béton, terre, herbe, sable, neige, eau, métal), atterrissage selon le sol, saut, boost (départ puis souffle en boucle tant qu'il boost), freinage, vent qui monte avec la vitesse, bang supersonique (boost relancé au-dessus de 200 km/h, choc contre une voiture au-dessus de 120 km/h). Les règles et les volumes sont ceux du mode superposé. Les sons suivent Sonic, même en boost.
- Plus riche que le superposé : toutes les variantes de Frontiers sont jouées au hasard (jusqu'à 12 bruits de pas différents sur la terre, au lieu d'un seul), et toutes les couches des sons à plusieurs couches (boost, saut, bang).
- Le convertisseur extrait ces sons de la banque de Frontiers du joueur en WAV (`mods/sonicnative_data/sounds`, 121 fichiers, 10 Mo) et calcule le moment où chaque pied touche le sol dans les animations de marche et de course. Les sons en boucle sont coupés à la boucle de Frontiers, avec un fondu de 20 ms à la jonction (sans lui, le souffle du boost faisait un petit clic à chaque tour).
- La détection du sol sous les pieds de Sonic, retirée en 0.1.0, revient (la même que le superposé).
- Sons coupés en pause et quand Sonic n'est pas à pied.
- `natif/build.ps1 -Install` met le mod à jour sur place au lieu de supprimer puis recopier son dossier : avec le jeu ouvert, BeamNG perdait la caméra de Sonic (image figée, commandes inversées).
- À savoir : le bang supersonique suit Sonic au lieu de rester sur place (les sons du mod sont accrochés à son corps).

## natif-v0.1.0
- Premier prototype du mode natif, sans DLL : Sonic est un vrai objet de la scène de BeamNG (un StaticShape), éclairé par le jeu et caché par ce qui passe devant lui. Il ne projette pas encore d'ombre.
- Un convertisseur lit Sonic dans le Sonic Frontiers du joueur (modèle, squelette, animations de repos, de marche, de course, de boost et de chute, textures) et écrit un modèle COLLADA animé, ses matériaux et ses textures dans le dossier utilisateur de BeamNG (`mods/sonicnative_data`). Rien de Frontiers n'est dans le dépôt ni dans le mod.
- Le mod `sonicnative` place Sonic aux pieds du personnage à pied de BeamNG, le tourne dans le sens de sa course, cache le personnage du jeu et choisit l'animation et sa vitesse selon son allure. La physique de Sonic (extension `sonicnative_body`) est reprise du mode superposé.
- `natif/build.ps1` vérifie le Lua, construit le convertisseur et installe le mod dans le dossier utilisateur de dev (ou dans le dossier normal).
- Testé dans BeamNG sur Gridmap V2 : Sonic apparaît texturé, à pied dès le chargement de la carte, avec sa caméra de poursuite ; course (jusqu'à environ 90 km/h), boost (près de 300 km/h, course sur un mur), saut (environ 3 m) avec l'animation de chute, montée et descente de voiture ; il est caché par une voiture placée devant lui.
- Si la carte a été lancée sans le mode marche de BeamNG (`gameplay_walk`), le mod le charge lui-même pour mettre Sonic à pied ; pendant les 3 s où le nouveau corps se met en place, la caméra de Sonic est remise si BeamNG la réinitialise (ensuite, la touche C reste au choix du joueur).
