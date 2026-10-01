struct Car { int speed; int gear; };
struct Driver { Car* car; int skill; };

int Boost(Driver* d, int amount)
{
    int boost = amount * d->skill;
    d->car->speed += boost;
    d->car->gear = 2;
    return d->car->speed;
}
