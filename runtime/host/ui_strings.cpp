// The launcher's and the settings panel's strings in the game's subtitle language.
// See ui_strings.h for why and for the font limits that shape the spellings.
//
// Four columns — English, French, Italian, Spanish — in the order of the Xbox
// language IDs 1, 4, 6, 5 (part 99 measured which banks the disc carries).
// Rules the translations follow, because the 5x7 font enforces them:
//   * capitals only, no accents (ECHAP, RESOLUTION, ESPANOL);
//   * no apostrophes or quotes ("MODE AFFICHAGE", not "MODE D'AFFICHAGE");
//   * a launcher label must stay under 23 characters (its value column starts at
//     x=300 with 12 px per character), a launcher line under ~57, a panel line
//     under ~52 — the English lines already sit at those widths.
// A missing entry is a bug, so the table is indexed by the enum and sized by it:
// a new UiStr without its four strings fails to compile.
#include "ui_strings.h"

#include <cstdio>

#include "settings.h"

namespace
{
struct Entry
{
    const char* en;
    const char* fr;
    const char* it;
    const char* es;
};

constexpr Entry kTable[] = {
    // Play
    {"PLAY", "JOUER", "GIOCA", "JUGAR"},
    // NotInstalledYet
    {"(GAME NOT INSTALLED YET)", "(JEU PAS ENCORE INSTALLE)", "(GIOCO NON ANCORA INSTALLATO)",
     "(JUEGO AUN NO INSTALADO)"},
    // DisplayMode
    {"DISPLAY MODE", "MODE AFFICHAGE", "MODO SCHERMO", "MODO PANTALLA"},
    // Resolution
    {"RESOLUTION", "RESOLUTION", "RISOLUZIONE", "RESOLUCION"},
    // VSync
    {"VSYNC", "VSYNC", "VSYNC", "VSYNC"},
    // Shadows
    {"SHADOWS", "OMBRES", "OMBRE", "SOMBRAS"},
    // Msaa
    {"MSAA", "MSAA", "MSAA", "MSAA"},
    // FpsCap
    {"FPS CAP", "LIMITE FPS", "LIMITE FPS", "LIMITE FPS"},
    // Fov
    {"FOV", "CHAMP DE VISION", "CAMPO VISIVO", "CAMPO DE VISION"},
    // Subtitles
    {"SUBTITLES", "SOUS-TITRES", "SOTTOTITOLI", "SUBTITULOS"},
    // SkipIntroLogos
    {"SKIP INTRO LOGOS", "PASSER LES LOGOS INTRO", "SALTA LOGHI INTRO", "SALTAR LOGOS INTRO"},
    // Window
    {"WINDOW", "FENETRE", "FINESTRA", "VENTANA"},
    // Borderless
    {"BORDERLESS", "SANS BORDURE", "SENZA BORDI", "SIN BORDES"},
    // Fullscreen
    {"FULLSCREEN", "PLEIN ECRAN", "SCHERMO INTERO", "PANTALLA COMPLETA"},
    // Off
    {"OFF", "NON", "NO", "NO"},
    // On
    {"ON", "OUI", "SI", "SI"},
    // Low
    {"LOW", "FAIBLE", "BASSA", "BAJA"},
    // Medium
    {"MEDIUM", "MOYEN", "MEDIA", "MEDIA"},
    // High
    {"HIGH", "ELEVE", "ALTA", "ALTA"},
    // RtLow
    {"RT LOW", "RT FAIBLE", "RT BASSA", "RT BAJA"},
    // RtMedium
    {"RT MEDIUM", "RT MOYEN", "RT MEDIA", "RT MEDIA"},
    // RtHigh
    {"RT HIGH", "RT ELEVE", "RT ALTA", "RT ALTA"},
    // Default
    {"DEFAULT", "DEFAUT", "PREDEFINITO", "PREDETERMINADO"},
    // LauncherHint
    {"UP/DOWN SELECT   LEFT/RIGHT CHANGE   ENTER PLAY",
     "HAUT/BAS CHOISIR   GAUCHE/DROITE MODIFIER   ENTREE JOUER",
     "SU/GIU SCEGLI   SINISTRA/DESTRA CAMBIA   INVIO GIOCA",
     "ARRIBA/ABAJO ELEGIR   IZQ/DER CAMBIAR   INTRO JUGAR"},
    // GameInstalled
    {"GAME INSTALLED", "JEU INSTALLE", "GIOCO INSTALLATO", "JUEGO INSTALADO"},
    // DropPackage
    {"DROP YOUR XBLA PACKAGE FILE ONTO THIS WINDOW TO INSTALL",
     "DEPOSEZ VOTRE FICHIER XBLA SUR CETTE FENETRE POUR INSTALLER",
     "TRASCINA IL FILE XBLA SU QUESTA FINESTRA PER INSTALLARE",
     "ARRASTRA TU ARCHIVO XBLA A ESTA VENTANA PARA INSTALAR"},
    // ControlsPad
    {"ARROWS / D-PAD MOVE   ENTER / A SELECT   ESC / B QUIT",
     "FLECHES/CROIX BOUGER   ENTREE/A CHOISIR   ECHAP/B QUITTER",
     "FRECCE/CROCE MUOVI   INVIO/A SCEGLI   ESC/B ESCI",
     "FLECHAS/CRUCETA MOVER   INTRO/A ELEGIR   ESC/B SALIR"},
    // ControlsKeyboard
    {"ARROWS MOVE   ENTER SELECT   ESC QUIT",
     "FLECHES BOUGER   ENTREE CHOISIR   ECHAP QUITTER",
     "FRECCE MUOVI   INVIO SCEGLI   ESC ESCI",
     "FLECHAS MOVER   INTRO ELEGIR   ESC SALIR"},
    // Installing
    {"INSTALLING", "INSTALLATION", "INSTALLAZIONE", "INSTALANDO"},
    // CopyingPackage
    {"COPYING PACKAGE...", "COPIE DU PAQUET...", "COPIA DEL PACCHETTO...",
     "COPIANDO EL PAQUETE..."},
    // Unpacking
    {"UNPACKING - %u OF %u MB", "EXTRACTION - %u SUR %u MO", "ESTRAZIONE - %u DI %u MB",
     "EXTRAYENDO - %u DE %u MB"},
    // InstalledPressEnter
    {"INSTALLED - PRESS ENTER TO PLAY", "INSTALLE - ENTREE POUR JOUER",
     "INSTALLATO - INVIO PER GIOCARE", "INSTALADO - INTRO PARA JUGAR"},
    // InstallFailed
    {"INSTALL FAILED: ", "ECHEC INSTALLATION: ", "INSTALLAZIONE FALLITA: ",
     "FALLO DE INSTALACION: "},
    // NotAPackage
    {"NOT AN XBOX 360 PACKAGE - IT BEGINS ", "PAS UN PAQUET XBOX 360 - IL COMMENCE PAR ",
     "NON E UN PACCHETTO XBOX 360 - INIZIA CON ", "NO ES UN PAQUETE XBOX 360 - EMPIEZA POR "},
    // CouldNotCopy
    {"COULD NOT COPY THE PACKAGE IN: ", "COPIE DU PAQUET IMPOSSIBLE: ",
     "IMPOSSIBILE COPIARE IL PACCHETTO: ", "NO SE PUDO COPIAR EL PAQUETE: "},
    // PcSettings
    {"PC SETTINGS", "PARAMETRES PC", "IMPOSTAZIONI PC", "AJUSTES PC"},
    // PanelHintKeyboard
    {"UP/DOWN ROW   LEFT/RIGHT CHANGE   X APPLY   ESC CLOSE",
     "HAUT/BAS LIGNE   G/D CHANGER   X APPLIQUER   ECHAP FERMER",
     "SU/GIU RIGA   SIN/DES CAMBIA   X APPLICA   ESC CHIUDI",
     "ARRIBA/ABAJO FILA   IZQ/DER CAMBIAR   X APLICAR   ESC CERRAR"},
    // PanelHintPad
    {"UP/DOWN ROW   LEFT/RIGHT CHANGE   X APPLY   B CLOSE",
     "HAUT/BAS LIGNE   G/D CHANGER   X APPLIQUER   B FERMER",
     "SU/GIU RIGA   SIN/DES CAMBIA   X APPLICA   B CHIUDI",
     "ARRIBA/ABAJO FILA   IZQ/DER CAMBIAR   X APLICAR   B CERRAR"},
    // Shadow
    {"SHADOW", "OMBRES", "OMBRE", "SOMBRAS"},
    // FrameCap
    {"FRAME CAP", "LIMITE FPS", "LIMITE FPS", "LIMITE FPS"},
    // FieldOfView
    {"FIELD OF VIEW", "CHAMP DE VISION", "CAMPO VISIVO", "CAMPO DE VISION"},
    // MouseSens
    {"MOUSE SENS", "SENSIB SOURIS", "SENSIB MOUSE", "SENSIB RATON"},
    // Exposure
    {"EXPOSURE", "EXPOSITION", "ESPOSIZIONE", "EXPOSICION"},
    // FooterApplyResolution
    {"PRESS X TO APPLY THE NEW RESOLUTION", "X POUR APPLIQUER LA NOUVELLE RESOLUTION",
     "PREMI X PER APPLICARE LA NUOVA RISOLUZIONE", "PULSA X PARA APLICAR LA NUEVA RESOLUCION"},
    // FooterMsaaNextLaunch
    {"MSAA APPLIES AT THE NEXT LAUNCH", "MSAA APPLIQUE AU PROCHAIN LANCEMENT",
     "MSAA SI APPLICA AL PROSSIMO AVVIO", "MSAA SE APLICA EN EL PROXIMO INICIO"},
    // FooterRtOff
    {"RESOLUTION: X APPLIES LIVE - RT SHADOWS ARE OFF IN THIS BUILD",
     "RESOLUTION: X APPLIQUE EN DIRECT - OMBRES RT DESACTIVEES",
     "RISOLUZIONE: X APPLICA SUBITO - OMBRE RT DISATTIVATE",
     "RESOLUCION: X APLICA EN VIVO - SOMBRAS RT DESACTIVADAS"},
    // FooterNoRayQuery
    {"RESOLUTION: X APPLIES LIVE - NO RAY QUERY: RT UNAVAILABLE",
     "RESOLUTION: X APPLIQUE EN DIRECT - PAS DE RAY QUERY: RT INDISPONIBLE",
     "RISOLUZIONE: X APPLICA SUBITO - NESSUN RAY QUERY: RT NON DISPONIBILE",
     "RESOLUCION: X APLICA EN VIVO - SIN RAY QUERY: RT NO DISPONIBLE"},
    // FooterNoRtCache
    {"RESOLUTION: X APPLIES LIVE - NO RT SHADER CACHE: SEE THE LOG",
     "RESOLUTION: X APPLIQUE EN DIRECT - PAS DE CACHE SHADER RT: VOIR LE LOG",
     "RISOLUZIONE: X APPLICA SUBITO - NESSUNA CACHE SHADER RT: VEDI IL LOG",
     "RESOLUCION: X APLICA EN VIVO - SIN CACHE DE SHADER RT: VER EL LOG"},
    // FooterShadowLive
    {"RESOLUTION: X APPLIES LIVE - SHADOW: LIVE",
     "RESOLUTION: X APPLIQUE EN DIRECT - OMBRES: EN DIRECT",
     "RISOLUZIONE: X APPLICA SUBITO - OMBRE: SUBITO",
     "RESOLUCION: X APLICA EN VIVO - SOMBRAS: EN VIVO"},
    // FooterShadowInert
    {"RESOLUTION: X APPLIES LIVE - SHADOW INERT AT 720P",
     "RESOLUTION: X APPLIQUE EN DIRECT - OMBRES INACTIVES EN 720P",
     "RISOLUZIONE: X APPLICA SUBITO - OMBRE INATTIVE A 720P",
     "RESOLUCION: X APLICA EN VIVO - SOMBRAS INACTIVAS A 720P"},
};
static_assert(sizeof(kTable) / sizeof(kTable[0]) == size_t(UiStr::Count),
              "every UiStr needs its four strings, in enum order");
} // namespace

int UiTextLanguage()
{
    switch (Settings_Language())
    {
        case 4: return 4;   // French
        case 6: return 6;   // Italian
        case 5: return 5;   // Spanish
        default: return 1;  // English — and Japanese (2) / Korean (7), which the
                            // bitmap font cannot draw
    }
}

const char* UiText(UiStr id)
{
    const size_t i = size_t(id);
    if (i >= size_t(UiStr::Count))
        return "?";
    const Entry& e = kTable[i];
    switch (UiTextLanguage())
    {
        case 4: return e.fr;
        case 6: return e.it;
        case 5: return e.es;
        default: return e.en;
    }
}
