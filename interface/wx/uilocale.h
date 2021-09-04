///////////////////////////////////////////////////////////////////////////////
// Name:        wx/uilocale.h
// Purpose:     Interface of wxUILocale
// Author:      Vadim Zeitlin
// Created:     2021-08-01
// Copyright:   (c) 2021 Vadim Zeitlin <vadim@wxwidgets.org>
// Licence:     wxWindows licence
///////////////////////////////////////////////////////////////////////////////

/**
    Flags for wxUILocale::CompareStrings() function.

    @since 3.1.6
 */
enum
{
    /// Compare strings case-sensitively, this is the default.
    wxCompare_CaseSensitive   = 0,

    /**
        Ignore strings case when comparing.

        Note that this flag is not supported under POSIX systems, where it is
        simply ignored.
     */
    wxCompare_CaseInsensitive = 1
};

/**
    Query and modify locale used for the UI by the current platform.

    UI locale determines all culture-dependent conventions used in the user
    interface, including numbers, currencies and dates formatting. It also
    determines the language used by the native dialogs, such as wxFileDialog,
    where different labels use the language corresponding to the current UI
    locale.

    The UI locale is, in general, different from C locale set by the standard
    @c setlocale() function and affecting C standard library functions such as
    @c printf(), @c scanf(), @c strftime() and many others. Unfortunately, the
    relationship between C and UI locales is not the same depending on the
    platform: with wxGTK they must be the same, but under macOS C locale must
    not be changed, as doing this exposes bugs in the system. Because of this,
    applications can't generally count on C locale being set to any particular
    value and it is best to avoid using it, including implicitly via the
    standard C functions, in portable code. Instead, consider using
    wxNumberFormatter for parsing and formatting numbers according to the
    current UI locale or wxString::FromCDouble() and wxString::ToCDouble()
    functions for doing it always using period as decimal separator.

    Localized applications should call wxUILocale::UseDefault() on startup to
    explicitly indicate that they opt-in using the current UI locale, even if
    this results in changing the global C locale, as is the case in wxGTK. Note
    that some platforms (MSW and macOS) will use default user locale for their
    standard dialogs even if this function is not called, but it is still
    necessary to call it to use the correct number and date formats and to
    avoid mixing messages in the user language with default formats not
    corresponding to it.

    Please also note that under macOS to really use the user locale, it must be
    listed as a supported language in the application @c Info.plist file under
    @c CFBundleLocalizations key.

    Unlike wxLocale class, this class doesn't affect the translations used by
    the application, see wxTranslations for doing this.

    @library{wxbase}
    @since 3.1.6
 */
class wxUILocale
{
public:
    /**
        Configure the UI to use the default user locale.

        Localized applications should call this functions as early as possible
        during the program startup, e.g. in the very beginning of the
        overridden wxApp::OnInit().

        Note that under most Unix systems (but not macOS) this function changes
        the C locale to the locale specified by the environment variables and
        so affects the results of calling C functions such as @c sprintf() etc
        which can use comma, rather than period, as decimal separator. The
        wxString::ToCDouble() and wxString::FromCDouble() functions can be used
        for parsing and formatting floating point numbers using period as
        decimal separator independently of the current locale.

        @return @true on success or @false if the default locale couldn't be set
     */
    static bool UseDefault();

    /**
        Get the object corresponding to the currently used locale.

        If UseDefault() had been called, this object corresponds to the default
        user locale. Otherwise it corresponds to a generic locale similar to
        "C" locale, i.e. always uses period as decimal separator and m/d/y date
        format.
     */
    static const wxUILocale& GetCurrent();

    /**
        Creates the locale corresponding to the given locale identifier.

        In the simplest case, this can be used as following:
        @code
            const wxUILocale loc(wxLocaleIdent("fr"));
        @endcode
        see wxLocaleIdent description for more details.

        If @a localeId is not recognized or not supported, default ("C") locale
        is used instead. Additionally, if @a localeId is empty (see
        wxLocaleIdent::IsEmpty()), an assertion failure is triggered.
     */
    explicit wxUILocale(const wxLocaleIdent& localeId);

    /**
        Compares two strings using comparison rules of this locale.

        This function is useful for sorting the strings in the order expected
        by the user, e.g. by correctly sorting "ä" in the same way as "a" when
        using German locale, but not when using Swedish one, in which "ä" is
        sorted after "z".

        It can be used both with the current locale, and with any other
        supported locale, even under systems not supporting changing the UI
        locale, such as macOS.

        @param lhs
            First comparing string.
        @param rhs
            Second comparing string.
        @param flags
            Can be used to specify whether to compare strings case-sensitively
            (default) or not, by specifying ::wxCompare_CaseInsensitive (note
            that this flag only works under MSW and Mac and is simply ignored
            under the other platforms).
        @return
            -1 if @a lhs is less than @a rhs.
            0 if @a lhs is equal to @a rhs.
            1 if @a lhs is greater than @a rhs.
     */
    int CompareStrings(const wxString& lhs, const wxString& rhs,
                       int flags = wxCompare_CaseSensitive) const;

    /**
        Get the platform-dependent name of the current locale.

        This name can be used in diagnostic messages.
     */
    wxString GetName() const;

    /**
        Query the locale for the specified information.

        This function returns the value of the locale-specific option specified
        by the given @a index.

        @param index
            One of the elements of wxLocaleInfo enum.
        @param cat
            The category to use with the given index or wxLOCALE_CAT_DEFAULT if
            the index can only apply to a single category.
        @return
            The option value or empty string if the function failed.
     */
    wxString GetInfo(wxLocaleInfo index,
                     wxLocaleCategory cat = wxLOCALE_CAT_DEFAULT) const;

    /**
        Return true if locale is supported on the current system.

        If this function returns @a false, the other functions of this class,
        such as GetInfo() and CompareStrings(), behave as in "C" locale, i.e.
        it's still safe to call them, but their results don't reflect the rules
        for the locale in question, but just use the default (i.e. US English)
        conventions.
     */
    bool IsSupported() const;
};

/**
    Return the format to use for formatting user-visible dates.

    This is a simple wrapper function normally calling wxUILocale::GetInfo()
    with wxLOCALE_SHORT_DATE_FMT argument, but which is also available when @c
    wxUSE_INTL==0, i.e. support for internationalization is disabled at
    compile-time, in which case it returns @c %x string, i.e. uses the current
    C locale formatting rather than UI locale.

    @see wxDateTime::Format()

    @since 3.1.6
 */
wxString wxGetUIDateFormat();

/**
    Represents a locale in a portable way.

    There are two possible ways to construct wxLocaleIdent:

        - You can either use fromTag() to create it from a string in the form
          @code language ["-" script] ["-" region] @endcode, corresponding to
          the subset of BCP 47 (https://www.rfc-editor.org/rfc/bcp/bcp47.txt)
          syntax.
        - Or you can create it from the different parts of this string by using
          the default constructor and then chaining calls to Language(),
          Region(), Script() and other methods.

    The first method is useful for interoperating with the other software using
    BCP 47 language tags, while the second one may may result in more readable
    code and allows to specify Unix-specific locale description parts such as
    charset and modifier that are not part of the BCP 47 strings.

    Example of using wxLocaleIdent in the second way:
    @code
      auto loc = wxLocaleIdent().Language("fr").Region("BE").Modifier("euro");
    #if defined(__WINDOWS__) || defined(__WXOSX__)
      wxASSERT( loc.GetName() == "fr_BE" );
    #elif defined(__UNIX__)
      wxASSERT( loc.GetName() == "fr_BE@euro" );
    #endif
    @endcode

    For the first way, it is enough to just write
    @code
      auto loc = wxLocaleIdent::FromTag("fr-BE"); // Dash, not underscore!
    @endcode

    @since 3.1.6
*/
class wxLocaleIdent
{
public:
    /**
        Return the locale identifier corresponding to the given BCP 47-like tag.

        The string must contain at least the language part (2 or 3 ASCII
        letters) and may contain script and region separated by dashes, i.e.
        all of the following are valid:

            - "mn"
            - "mn-MN"
            - "mn-Cyrl-MN"

        Note that while BCP 47 extlangs, variants, extensions, private use and
        grandfathered tags are currently not directly supported, they may still
        work for creating wxUILocale on platforms with native support for BCP
        47 strings.

        If the input argument uses an unrecognized syntax (e.g. is empty), an
        empty wxLocaleIdent is returned. Of course, even if this function
        returns a non-empty object, the resulting locale may still be invalid
        or unsupported, use wxUILocale::IsSupported() to check for this.
     */
    static wxLocaleIdent FromTag(const wxString& tag);

    /**
        Default constructor creates an empty and invalid locale identifier.

        At least Language() must be called to make the identifier valid.
    */
    wxLocaleIdent();

    /**
        Set language.

        Return reference to @this for method chaining.

        See https://www.loc.gov/standards/iso639-2/php/English_list.php for the
        list of all language codes.

        @param language
            A two-letter ISO 639-1 language code or a three-letter ISO 639-2
            code for the languages without ISO 639-1 codes.
    */
    wxLocaleIdent& Language(const wxString& language);

    /**
        Set region.
        Return reference to @this for method chaining.

        @param region
            It specifies an uppercase ISO 3166-1 country/region identifier.
    */
    wxLocaleIdent& Region(const wxString& region);

    /**
        Set script.

        Note that script value is currently ignored under Unix systems.

        Return reference to @this for method chaining.

        @param script
            It is an initial-uppercase ISO 15924 script code.
    */
    wxLocaleIdent& Script(const wxString& script);

    /**
        Set charset.

        Note that this value is only used under Unix systems and simply ignored
        under the other ones.

        Return reference to @this for method chaining.

        @param charset
            Charset is a string such as "UTF-8", "ISO855915" or "KOI8R".
            Supported charsets depend on the implementation and installation.
    */
    wxLocaleIdent& Charset(const wxString& charset);

    /**
        Set modifier.

        Note that this value is only used under Unix systems and simply ignored
        under the other ones.

        Return reference to @this for method chaining.

        @param modifier
            Modifier is defined by ISO/IEC 15897.
            It is a semi-colon separated list of identifiers, or name=value pairs.
    */
    wxLocaleIdent& Modifier(const wxString& modifier);

    /// Return the language part of the locale identifier.
    const wxString& GetLanguage() const;

    /// Return the region part of the locale identifier.
    const wxString& GetRegion() const;

    /// Return the script part of the locale identifier.
    const wxString& GetScript() const;

    /// Return the charset part of the locale identifier.
    const wxString& GetCharset() const;

    /// Return the modifier part of the locale identifier.
    const wxString& GetModifier() const;

    /**
        Construct platform dependent name.
        Format:
        Windows: <language>-<script>-<REGION>
        Unix:    <language>_<REGION>.<charset>@<modifier>
        MacOS:   <language>-<script>_<REGION>
    */
    wxString GetName() const;

    /**
        Check if the locale is empty.

        @return @true if this is an empty, invalid object.
    */
    bool IsEmpty() const;
};
